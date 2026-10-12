// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Playlist.h"
#include "LookAndFeel.h"
#include "model/Clips.h"
#include "model/LiveInput.h"
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
    constexpr int dragThreshold = 4;  // pixels the mouse moves before a click becomes a drag

    constexpr double wheelNotch = 60.0 / 256.0; // one notch of a mouse wheel, as JUCE reports it on Windows
    constexpr double zoomPerNotch = 1.25;
    constexpr double scrollPerNotch = 48.0; // pixels
    constexpr double zoomStep = 1.5;        // for the zoom commands
    constexpr double minLineSpacing = 6.0;  // grid lines closer than this many pixels aren't drawn

    const juce::String emptySpaceHint ("Click to select the track, drag to select a time range, and double-click on a track with an "
                                       "instrument to add a MIDI clip. Ctrl+wheel zooms; Alt+wheel resizes the tracks.");

    bool isTrackState (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::TRACK) || state.hasType (te::IDs::FOLDERTRACK);
    }

    // The clip a state belongs to (a note belongs to its clip, say), or an invalid tree.
    juce::ValueTree findClipState (juce::ValueTree state)
    {
        while (state.isValid() && ! te::Clip::isClipState (state) && ! isTrackState (state))
            state = state.getParent();

        return te::Clip::isClipState (state) ? state : juce::ValueTree();
    }

    juce::MouseCursor::StandardCursorType getCursorFor (ClipZone zone)
    {
        switch (zone)
        {
            case ClipZone::trimStart:
            case ClipZone::trimEnd:
                return juce::MouseCursor::LeftRightResizeCursor;
            case ClipZone::fadeIn:
            case ClipZone::fadeOut:
                return juce::MouseCursor::CrosshairCursor;
            case ClipZone::selectRange:
                return juce::MouseCursor::IBeamCursor;
            case ClipZone::move:
            default:
                return juce::MouseCursor::DraggingHandCursor;
        }
    }

    juce::MouseCursor::StandardCursorType getCursorFor (Tool tool)
    {
        switch (tool)
        {
            case Tool::draw:
            case Tool::paint:
            case Tool::select:
            case Tool::zoom:
                return juce::MouseCursor::CrosshairCursor;
            case Tool::slice:
                return juce::MouseCursor::IBeamCursor;
            case Tool::slip:
                return juce::MouseCursor::LeftRightResizeCursor;
            case Tool::mute:
            case Tool::erase:
                return juce::MouseCursor::PointingHandCursor;
            case Tool::smart:
            default:
                return juce::MouseCursor::NormalCursor;
        }
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
        const te::BeatRange visible (timeline.xToBeat (clip.getX() - 1), timeline.xToBeat (clip.getRight() + 1));

        // Ticks: tall for bars, shorter for beats, shortest between them.
        const auto tickType = getAdaptiveSnapType (tempo, visible.getStart(), timeline.pixelsPerBeat, false, minLineSpacing);

        forEachGridLine (tempo, tickType, false, visible,
                         [&] (te::BeatPosition beat, GridLineType type)
                         {
                             const auto isBar = type == GridLineType::bar;
                             const auto tick = isBar ? getHeight() / 2 : (type == GridLineType::beat ? getHeight() / 4 : getHeight() / 8);
                             g.setColour (isBar ? p.textMuted : p.textMuted.withAlpha (0.4f));
                             g.fillRect (juce::roundToInt (timeline.beatToX (beat)), getHeight() - 1 - tick, 1, tick);
                         });

        // Bar numbers, as far apart as they need to be to read: every bar, or every 2nd, 4th, ...
        const auto barWidth = timeline.pixelsPerBeat * tempo.getTimeSigAt (visible.getStart()).numerator.get();
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
// The tracks' rows, with the grid, and the clips on them. The hint bar shows what the mouse does where
// it is (the description changes as it moves).
struct Playlist::Lanes : public juce::Component
{
    explicit Lanes (Playlist& p)
        : playlist (p)
    {
        setTitle ("Tracks");
        setDescription (emptySpaceHint);
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
        const te::BeatRange visible (timeline.xToBeat (clip.getX() - 1), timeline.xToBeat (clip.getRight() + 1));
        auto type = playlist.getSnapType();

        if (getGridStepInBeats (type, tempo, visible.getStart(), triplets) * timeline.pixelsPerBeat < minLineSpacing)
            type = getAdaptiveSnapType (tempo, visible.getStart(), timeline.pixelsPerBeat, triplets, minLineSpacing);

        const auto barColour = p.border.interpolatedWith (p.textMuted, 0.3f);
        const auto subdivisionColour = p.border.withMultipliedAlpha (0.5f);

        forEachGridLine (tempo, type, triplets, visible,
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

    // Over the clips: the time range selected across tracks, and the Select or Zoom tool's rectangle.
    void paintOverChildren (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);

        if (! playlist.range.isEmpty())
        {
            auto firstRow = (int) playlist.rows.size(), lastRow = -1;

            for (auto* track : playlist.range.getTracks())
            {
                if (const auto row = playlist.getRowOf (track); row >= 0)
                {
                    firstRow = std::min (firstRow, row);
                    lastRow = std::max (lastRow, row);
                }
            }

            if (lastRow >= 0)
            {
                const auto& first = playlist.rows[(size_t) firstRow];
                const auto& last = playlist.rows[(size_t) lastRow];
                const auto left = juce::roundToInt (playlist.view.beatToX (playlist.range.getRange().getStart()));
                const auto right = juce::roundToInt (playlist.view.beatToX (playlist.range.getRange().getEnd()));
                const juce::Rectangle<int> area (left, first.top - playlist.scrollY, std::max (1, right - left),
                                                 last.top + last.height - first.top);

                g.setColour (p.accent.withAlpha (0.25f));
                g.fillRect (area);
                g.setColour (p.accent);
                g.drawRect (area, 1);
            }
        }

        if (! playlist.rubberBand.isEmpty())
        {
            g.setColour (p.accent.withAlpha (0.15f));
            g.fillRect (playlist.rubberBand);
            g.setColour (p.accent);
            g.drawRect (playlist.rubberBand, 1);
        }
    }

    // The tools' work is the playlist's.
    void mouseMove (const juce::MouseEvent& e) override { playlist.lanesMouseMove (e); }
    void mouseDown (const juce::MouseEvent& e) override { playlist.lanesMouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { playlist.lanesMouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { playlist.lanesMouseUp (e); }
    void mouseDoubleClick (const juce::MouseEvent& e) override { playlist.lanesDoubleClick (e); }

    Playlist& playlist;
};

//==============================================================================
// What the mouse is doing in the lanes, from the moment it goes down until it goes up.
struct Playlist::Gesture
{
    enum class Kind
    {
        pending, // not dragging yet: a click, unless the mouse moves far enough
        move,
        trimStart,
        trimEnd,
        fadeIn,
        fadeOut,
        selectRange,
        crossfade,
        slip,   // the Slip tool
        paint,  // the Paint tool
        mute,   // the Mute tool, over clips
        erase,  // the Delete tool (or right-click, in FL Studio's keymap), over clips
        select, // the Select tool's rectangle
        zoom    // the Zoom tool's range
    };

    // A clip being moved, and where it started.
    struct MovingClip
    {
        te::SafeSelectable<te::Clip> clip;
        te::BeatPosition start;
        int row = -1;
    };

    Kind kind = Kind::pending;
    te::SafeSelectable<te::Clip> clip; // the clip the mouse went down on, if any
    std::optional<ClipZone> zone;      // and where on it
    te::BeatPosition startBeat;        // the beat under the mouse when it went down
    int startRow = -1;
    bool clickOnMouseUp = false; // the click's selection waits for the mouse to go up without a drag

    std::vector<MovingClip> moving;
    te::BeatPosition anchorStart; // where the clip under the mouse (or its copy) started; it is the one that snaps
    int rowOffset = 0;            // how many rows the moving clips have moved

    std::optional<CrossfadeSpot> crossfade;                         // where the mouse went down to draw a crossfade
    juce::Point<int> startPosition;                                 // in the lanes
    te::TimeDuration startOffset;                                   // Slip: the clip's offset when the drag began
    te::BeatPosition nextPaintStart;                                // Paint: where the next copy goes
    std::vector<te::SafeSelectable<te::Clip>> done;                 // Mute and Delete: the clips already done
    std::vector<te::SafeSelectable<te::Selectable>> selectedBefore; // Select with Ctrl: what was selected already
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
Playlist::Playlist (CommandRegistry& registry, te::SelectionManager& selectionManager, RangeSelection& rangeSelection)
    : commands (registry),
      selection (selectionManager),
      range (rangeSelection),
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

    // The tools, one button each; the commands set them, so the keys and the buttons agree.
    const Icon toolIcons[] = { Icon::smartTool, Icon::draw,  Icon::paint,  Icon::slice, Icon::slip,
                               Icon::mute,      Icon::erase, Icon::select, Icon::zoom };

    for (size_t i = 0; i < std::size (allTools); ++i)
    {
        const auto toolToUse = allTools[i];
        auto* button = toolButtons.add (new IconButton (getToolName (toolToUse) + (toolToUse == Tool::smart ? "" : " tool"), toolIcons[i]));
        button->setRadioGroupId (1);
        button->setClickingTogglesState (true);
        button->onClick = [this, toolToUse] { commands.invoke (getToolCommandID (toolToUse)); };
        addAndMakeVisible (button);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &snapButton, &gridButton, ruler.get(), &headerColumn,
                                                             lanes.get(), &verticalScrollBar, &horizontalScrollBar, playhead.get() })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &snapButton, &gridButton })
        c->setEnabled (false); // until a project is open

    selection.addChangeListener (this);
    range.addChangeListener (this);
    updateGridControls();
    setTool (Tool::smart);
}

Playlist::~Playlist()
{
    range.removeChangeListener (this);
    selection.removeChangeListener (this);
    editState.removeListener (this);
    headers.clear();        // before headerContent, which holds them
    clipComponents.clear(); // and the lanes
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
    lastClip = nullptr;
    gesture = nullptr;
    rubberBand = {};
    grid = newEdit != nullptr ? GridSettings::load (findPhaziteState (*newEdit)) : GridSettings();
    headers.clear();
    clipComponents.clear();
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

    rebuildClips();
    layOutRows();
    updateHeaders();
    updateClips();
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
    layOutClips();
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
void Playlist::rebuildClips()
{
    // The components of clips that are still there are kept; the others go.
    juce::OwnedArray<ClipComponent> kept;

    for (const auto& row : rows)
    {
        auto* clipTrack = dynamic_cast<te::ClipTrack*> (row.track.get());

        if (clipTrack == nullptr)
            continue;

        for (auto* clip : clipTrack->getClips())
        {
            auto* component = getClipComponentFor (clip->state);

            if (component != nullptr)
            {
                clipComponents.removeObject (component, false);
            }
            else
            {
                component = new ClipComponent (*clip);
                lanes->addAndMakeVisible (component);
            }

            kept.add (component);
        }
    }

    clipComponents.swapWith (kept);
}

void Playlist::layOutClips()
{
    for (auto* component : clipComponents)
        if (auto* clip = component->getClip())
            component->setBounds (getClipBounds (*clip));
}

void Playlist::updateClips()
{
    for (auto* component : clipComponents)
    {
        if (auto* clip = component->getClip())
        {
            const auto row = getRowOf (clip->getTrack());
            const auto trackColour = row >= 0 ? rows[(size_t) row].colour : juce::Colours::grey;
            component->update (getClipColour (*clip, trackColour), selection.isSelected (clip));
        }
    }
}

juce::Rectangle<int> Playlist::getClipBounds (te::Clip& clip) const
{
    const auto row = getRowOf (clip.getTrack());

    if (row < 0)
        return {};

    const auto& r = rows[(size_t) row];
    const auto left = juce::roundToInt (view.beatToX (clip.getStartBeat()));
    const auto right = juce::roundToInt (view.beatToX (clip.getEndBeat()));
    return { left, r.top - scrollY + 1, std::max (2, right - left), std::max (1, r.height - 3) };
}

ClipComponent* Playlist::getClipComponentAt (juce::Point<int> position) const
{
    // The last one added is drawn on top, so it is the one that is clicked.
    for (auto i = clipComponents.size(); --i >= 0;)
        if (auto* component = clipComponents.getUnchecked (i);
            component->getClip() != nullptr && component->getBounds().contains (position))
            return component;

    return nullptr;
}

ClipComponent* Playlist::getClipComponentFor (const juce::ValueTree& clipState) const
{
    for (auto* component : clipComponents)
        if (auto* clip = component->getClip(); clip != nullptr && clip->state == clipState)
            return component;

    return nullptr;
}

void Playlist::clipChanged (const juce::ValueTree& clipState)
{
    if (auto* component = getClipComponentFor (clipState))
    {
        if (auto* clip = component->getClip())
        {
            component->setBounds (getClipBounds (*clip));
            const auto row = getRowOf (clip->getTrack());
            component->update (getClipColour (*clip, row >= 0 ? rows[(size_t) row].colour : juce::Colours::grey),
                               selection.isSelected (clip));
        }
    }
}

void Playlist::clipClicked (te::Clip& clip, const juce::ModifierKeys& mods)
{
    range.clear();

    // Ctrl or Shift adds the clip to the selection, or takes it out.
    if (! (mods.isCommandDown() || mods.isShiftDown()))
        selection.selectOnly (clip);
    else if (selection.isSelected (clip))
        selection.deselect (&clip);
    else
        selection.addToSelection (clip);
}

void Playlist::showClipMenu()
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "edit.split", "edit.duplicate", "clip.duplicateAsAlias", "clip.makeUnique" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "clip.mute", "clip.quantize" });

    juce::PopupMenu fadeShapes;
    addCommandItems (fadeShapes, commands, { "clip.fadeLinear", "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve" });
    menu.addSubMenu ("Fade Shape", fadeShapes);

    menu.addSeparator();
    addCommandItems (menu, commands, { "edit.delete", "edit.deleteAndCloseGap" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::selectRange (te::BeatPosition from, te::BeatPosition to, int fromRow, int toRow)
{
    juce::Array<te::Track*> tracks;

    for (auto row = std::min (fromRow, toRow); row <= std::max (fromRow, toRow); ++row)
        if (juce::isPositiveAndBelow (row, (int) rows.size()))
            if (auto* track = rows[(size_t) row].track.get())
                tracks.add (track);

    range.set (te::BeatRange (std::min (from, to), std::max (from, to)), tracks);
}

std::optional<Playlist::CrossfadeSpot> Playlist::findCrossfadeAt (juce::Point<int> position) const
{
    auto* e = edit.get();
    const auto row = getRowAt (position.y);

    if (e == nullptr || row < 0)
        return {};

    // Only the lower third of a row, so the upper part still trims.
    const auto& r = rows[(size_t) row];
    auto* track = dynamic_cast<te::ClipTrack*> (r.track.get());

    if (track == nullptr || position.y + scrollY < r.top + r.height * 2 / 3)
        return {};

    const auto xOf = [this, e] (te::TimePosition time) { return view.beatToX (e->tempoSequence.toBeats (time)); };

    for (auto* a : track->getClips())
    {
        for (auto* b : track->getClips())
        {
            auto* first = dynamic_cast<te::AudioClipBase*> (a);
            auto* second = dynamic_cast<te::AudioClipBase*> (b);

            if (first == nullptr || second == nullptr || first == second)
                continue;

            // The second starts after the first, where it ends or inside it (a crossfade already), and ends later.
            const auto one = first->getEditTimeRange(), two = second->getEditTimeRange();

            if (two.getStart() <= one.getStart() || two.getEnd() <= one.getEnd() || xOf (two.getStart()) > xOf (one.getEnd()) + 1.0)
                continue;

            const auto boundary = te::TimePosition::fromSeconds ((one.getEnd().inSeconds() + two.getStart().inSeconds()) / 2.0);

            if (std::abs (xOf (boundary) - position.x) <= 6.0)
                return CrossfadeSpot { first, second, boundary };
        }
    }

    return {};
}

te::Clip* Playlist::placeClip (int row, te::BeatPosition beat, bool snapStart)
{
    auto* ed = edit.get();
    auto* track =
        juce::isPositiveAndBelow (row, (int) rows.size()) ? dynamic_cast<te::ClipTrack*> (rows[(size_t) row].track.get()) : nullptr;

    if (ed == nullptr || track == nullptr)
        return nullptr;

    // From the grid line at or before the beat.
    const auto& tempo = ed->tempoSequence;
    const auto start = snapStart ? tempo.toBeats (getSnapType().roundTimeDown (tempo.toTime (beat), tempo, grid.triplets)) : beat;

    // A copy of the last clip used: of a MIDI clip, an alias, as FL Studio places the same pattern again.
    if (auto* source = lastClip.get(); source != nullptr && te::containsClip (*ed, source))
    {
        const auto copies = copyClips ({ source }, dynamic_cast<te::MidiClip*> (source) != nullptr);

        if (copies.isEmpty())
            return nullptr;

        moveClip (*copies.getFirst(), start, *track);
        return copies.getFirst();
    }

    // No clip used yet: an empty MIDI clip, a bar long, on a track with an instrument.
    if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track); audioTrack != nullptr && hasInstrument (*audioTrack))
        return insertMidiClip (*track, { start, start + te::BeatDuration::fromBeats (tempo.getTimeSigAt (start).numerator.get()) },
                               nullptr);

    return nullptr;
}

void Playlist::muteOrDeleteClipAt (juce::Point<int> position, bool deleteIt)
{
    auto* component = getClipComponentAt (position);
    auto* clip = component != nullptr ? component->getClip() : nullptr;

    if (clip == nullptr || gesture == nullptr)
        return;

    // Each clip once per drag, however long the mouse stays on it.
    for (const auto& done : gesture->done)
        if (done.get() == clip)
            return;

    gesture->done.emplace_back (clip);

    if (deleteIt)
        clip->removeFromParent();
    else
        toggleClipsMuted ({ clip });
}

void Playlist::selectClipsIn (juce::Rectangle<int> area, bool addToSelection)
{
    juce::Array<te::Selectable*> items;

    if (addToSelection && gesture != nullptr)
        for (const auto& before : gesture->selectedBefore)
            if (auto* item = before.get())
                items.add (item);

    for (auto* component : clipComponents)
        if (auto* clip = component->getClip(); clip != nullptr && component->getBounds().intersects (area))
            items.addIfNotAlreadyThere (clip);

    range.clear();
    selection.deselectAll();

    for (auto* item : items)
        selection.addToSelection (item);
}

void Playlist::nudge (int direction)
{
    auto* ed = edit.get();

    if (ed == nullptr)
        return;

    // A step of the grid as it is now, so what is seen is what moves.
    const auto step =
        te::BeatDuration::fromBeats (direction * getGridStepInBeats (getSnapType(), ed->tempoSequence, view.start, grid.triplets));

    if (const auto clips = getSelectedClips (selection); ! clips.isEmpty())
    {
        nudgeClips (clips, step);
        return;
    }

    if (! range.isEmpty())
    {
        const auto r = range.getRange();
        const auto start = te::BeatPosition::fromBeats (std::max (0.0, (r.getStart() + step).inBeats()));
        range.set ({ start, start + r.getLength() }, range.getTracks());
    }
}

void Playlist::quantizeSelectedClips()
{
    auto* ed = edit.get();

    if (ed == nullptr)
        return;

    for (auto* clip : getSelectedClips (selection))
        if (auto* track = clip->getClipTrack())
            moveClip (*clip, snapToGrid (clip->getStartBeat(), getSnapType(), ed->tempoSequence, grid.triplets), *track);
}

//==============================================================================
void Playlist::setTool (Tool newTool)
{
    tool = newTool;
    gesture = nullptr;
    rubberBand = {};

    for (size_t i = 0; i < std::size (allTools); ++i)
        toolButtons[(int) i]->setToggleState (allTools[i] == tool, juce::dontSendNotification);

    lanes->setMouseCursor (getCursorFor (tool));
    lanes->setDescription (tool == Tool::smart ? emptySpaceHint : describeTool (tool));
    lanes->repaint();
}

void Playlist::lanesMouseMove (const juce::MouseEvent& e)
{
    // The hint bar and the pointer say what the mouse does here. With the Smart Tool, and with Draw over a
    // clip, that depends on where on the clip it is.
    if (tool == Tool::smart && findCrossfadeAt (e.getPosition()).has_value())
    {
        lanes->setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        lanes->setDescription ("Drag to crossfade the two clips: each plays over the other while one fades out and the other in.");
        return;
    }

    auto* component = getClipComponentAt (e.getPosition());

    if (component != nullptr && (tool == Tool::smart || tool == Tool::draw))
    {
        const auto zone =
            getClipZone (component->getBounds().toFloat(), e.position, dynamic_cast<te::AudioClipBase*> (component->getClip()) != nullptr);
        lanes->setMouseCursor (getCursorFor (zone));
        lanes->setDescription (describeClipZone (zone));
        return;
    }

    lanes->setMouseCursor (getCursorFor (tool));
    lanes->setDescription (tool == Tool::smart ? emptySpaceHint : describeTool (tool));
}

void Playlist::lanesMouseDown (const juce::MouseEvent& e)
{
    gesture = nullptr;
    auto* ed = edit.get();

    if (ed == nullptr)
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startPosition = e.getPosition();
    gesture->startBeat = view.xToBeat (e.position.x);
    gesture->startRow = getRowAt (e.y);

    auto* component = getClipComponentAt (e.getPosition());
    auto* clip = component != nullptr ? component->getClip() : nullptr;
    auto& undo = ed->getUndoManager();

    if (e.mods.isPopupMenu())
    {
        // In FL Studio's keymap, right-click deletes, as in FL Studio: this clip and those the mouse moves on to.
        if (commands.getKeymap().rightClickDeletes)
        {
            undo.beginNewTransaction ("Delete Clips");
            gesture->kind = Gesture::Kind::erase;
            muteOrDeleteClipAt (e.getPosition(), true);
        }
        else
        {
            smartToolDown (e); // the menus
        }

        return;
    }

    switch (tool)
    {
        case Tool::draw:
        case Tool::paint:
        {
            // Draw drags clips as the Smart Tool does; Paint selects them.
            if (clip != nullptr)
            {
                if (tool == Tool::draw)
                {
                    smartToolDown (e);
                }
                else
                {
                    clipClicked (*clip, e.mods);
                    gesture = nullptr;
                }

                return;
            }

            range.clear();
            undo.beginNewTransaction (tool == Tool::draw ? "Place Clip" : "Paint Clips");
            auto* placed = placeClip (gesture->startRow, gesture->startBeat, grid.snap && ! e.mods.isAltDown());

            if (placed == nullptr)
            {
                gesture = nullptr;
                return;
            }

            selection.selectOnly (placed);

            if (tool == Tool::draw)
            {
                // A drag that follows moves the new clip.
                gesture->kind = Gesture::Kind::move;
                gesture->clip = placed;
                gesture->anchorStart = placed->getStartBeat();
                gesture->moving.push_back ({ placed, placed->getStartBeat(), gesture->startRow });
            }
            else
            {
                gesture->kind = Gesture::Kind::paint;
                gesture->nextPaintStart = placed->getEndBeat();
            }

            return;
        }

        case Tool::slice:
            if (clip != nullptr)
            {
                undo.beginNewTransaction ("Split Clip");
                splitClips ({ clip }, snap (gesture->startBeat, e.mods));
            }

            gesture = nullptr;
            return;

        case Tool::slip:
            if (clip == nullptr)
            {
                gesture = nullptr;
                return;
            }

            clipClicked (*clip, {});
            undo.beginNewTransaction ("Slip Clip");
            gesture->kind = Gesture::Kind::slip;
            gesture->clip = clip;
            gesture->startOffset = clip->getPosition().getOffset();
            return;

        case Tool::mute:
        case Tool::erase:
            undo.beginNewTransaction (tool == Tool::mute ? "Mute Clips" : "Delete Clips");
            gesture->kind = tool == Tool::mute ? Gesture::Kind::mute : Gesture::Kind::erase;
            muteOrDeleteClipAt (e.getPosition(), tool == Tool::erase);
            return;

        case Tool::select:
            // A click selects (when the mouse goes up); a drag draws a rectangle.
            gesture->clip = clip;
            gesture->clickOnMouseUp = true;
            return;

        case Tool::zoom:
            return; // a click or a drag, decided later

        case Tool::smart:
        default:
            smartToolDown (e);
            return;
    }
}

void Playlist::smartToolDown (const juce::MouseEvent& e)
{
    // Where two audio clips meet: a drag draws a crossfade.
    if (! e.mods.isPopupMenu())
    {
        if (auto spot = findCrossfadeAt (e.getPosition()))
        {
            gesture->crossfade = spot;
            return;
        }
    }

    // On a clip: where it is clicked decides what a drag does.
    if (auto* component = getClipComponentAt (e.getPosition()))
    {
        auto& clip = *component->getClip();
        gesture->clip = &clip;
        gesture->zone = getClipZone (component->getBounds().toFloat(), e.position, dynamic_cast<te::AudioClipBase*> (&clip) != nullptr);

        if (e.mods.isPopupMenu())
        {
            if (! selection.isSelected (clip))
                clipClicked (clip, {});

            gesture = nullptr;
            showClipMenu();
            return;
        }

        // A plain click on a clip that isn't selected selects it at once, so a drag moves it. Other clicks
        // wait to see whether they become drags: a selected clip's drag moves the whole selection, and a
        // Ctrl-drag copies rather than selecting.
        if (! selection.isSelected (clip) && ! (e.mods.isCommandDown() || e.mods.isShiftDown()))
            clipClicked (clip, {});
        else
            gesture->clickOnMouseUp = true;

        return;
    }

    // On empty space: the track is selected, and a drag selects a time range.
    range.clear();

    if (gesture->startRow < 0)
    {
        selection.deselectAll();
        return;
    }

    if (auto* track = rows[(size_t) gesture->startRow].track.get())
    {
        if (e.mods.isPopupMenu())
        {
            if (! selection.isSelected (track))
                trackClicked (*track, {});

            gesture = nullptr;
            showTrackMenu (*track);
            return;
        }

        trackClicked (*track, e.mods);
    }
}

void Playlist::lanesMouseDrag (const juce::MouseEvent& e)
{
    auto* ed = edit.get();

    if (gesture == nullptr || ed == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        if (e.getDistanceFromDragStart() < dragThreshold)
            return;

        beginDrag (e);
    }

    const auto beat = view.xToBeat (e.position.x);
    auto* clip = gesture->clip.get();

    switch (gesture->kind)
    {
        case Gesture::Kind::move:
        {
            // The clip under the mouse snaps to the grid, and the others move with it, but no clip moves
            // before the start of the song.
            auto delta = snap (gesture->anchorStart + (beat - gesture->startBeat), e.mods) - gesture->anchorStart;

            for (const auto& moving : gesture->moving)
                delta = std::max (delta, te::BeatPosition() - moving.start);

            // Up or down by whole rows, as long as every clip lands on a track that holds clips.
            if (const auto row = getRowAt (e.y); row >= 0 && gesture->startRow >= 0)
            {
                const auto offset = row - gesture->startRow;
                const auto fits = std::all_of (gesture->moving.begin(), gesture->moving.end(),
                                               [this, offset] (const Gesture::MovingClip& moving)
                                               {
                                                   const auto target = moving.row + offset;
                                                   return juce::isPositiveAndBelow (target, (int) rows.size())
                                                          && dynamic_cast<te::ClipTrack*> (rows[(size_t) target].track.get()) != nullptr;
                                               });

                if (fits)
                    gesture->rowOffset = offset;
            }

            for (const auto& moving : gesture->moving)
            {
                const auto row = moving.row + gesture->rowOffset;

                if (auto* c = moving.clip.get(); c != nullptr && juce::isPositiveAndBelow (row, (int) rows.size()))
                    if (auto* track = dynamic_cast<te::ClipTrack*> (rows[(size_t) row].track.get()))
                        moveClip (*c, moving.start + delta, *track);
            }

            break;
        }

        case Gesture::Kind::trimStart:
            if (clip != nullptr)
                trimClipStart (*clip, snap (beat, e.mods));

            break;

        case Gesture::Kind::trimEnd:
            if (clip != nullptr)
                trimClipEnd (*clip, snap (beat, e.mods));

            break;

        case Gesture::Kind::fadeIn:
        case Gesture::Kind::fadeOut:
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
            {
                const auto time = ed->tempoSequence.toTime (beat);
                const auto fadeIn = gesture->kind == Gesture::Kind::fadeIn;
                setClipFade (*audio, fadeIn, fadeIn ? time - audio->getPosition().getStart() : audio->getPosition().getEnd() - time);
            }

            break;

        case Gesture::Kind::selectRange:
        {
            // From where the drag started to the pointer, across the rows between them.
            const auto lastRow = (int) rows.size() - 1;
            const auto from = snap (gesture->startBeat, e.mods), to = snap (beat, e.mods);
            const auto row = getRowAt (e.y) >= 0 ? getRowAt (e.y) : (e.y < 0 ? 0 : lastRow);

            if (lastRow >= 0 && from != to)
                selectRange (from, to, gesture->startRow >= 0 ? gesture->startRow : lastRow, row);

            break;
        }

        case Gesture::Kind::crossfade:
        {
            // As long as twice the distance from the boundary to the mouse: the boundary stays in the middle.
            const auto& spot = *gesture->crossfade;
            auto* first = dynamic_cast<te::AudioClipBase*> (spot.first.get());
            auto* second = dynamic_cast<te::AudioClipBase*> (spot.second.get());

            if (first != nullptr && second != nullptr)
            {
                const auto distance = ed->tempoSequence.toTime (beat) - spot.boundary;
                setCrossfade (*first, *second, spot.boundary, te::TimeDuration::fromSeconds (2.0 * std::abs (distance.inSeconds())));
            }

            break;
        }

        case Gesture::Kind::slip:
            if (clip != nullptr)
            {
                // What is inside follows the mouse; the clip stays where it is.
                const auto& tempo = ed->tempoSequence;
                clip->setOffset (gesture->startOffset
                                 - (tempo.toTime (snap (beat, e.mods)) - tempo.toTime (snap (gesture->startBeat, e.mods))));
            }

            break;

        case Gesture::Kind::paint:
            // Another copy each time the mouse passes the end of the last one.
            while (beat >= gesture->nextPaintStart)
            {
                auto* placed = placeClip (gesture->startRow, gesture->nextPaintStart, false);

                if (placed == nullptr)
                    break;

                selection.addToSelection (placed);
                gesture->nextPaintStart = placed->getEndBeat();
            }

            break;

        case Gesture::Kind::mute:
        case Gesture::Kind::erase:
            muteOrDeleteClipAt (e.getPosition(), gesture->kind == Gesture::Kind::erase);
            break;

        case Gesture::Kind::select:
            rubberBand = juce::Rectangle<int> (gesture->startPosition, e.getPosition());
            selectClipsIn (rubberBand, e.mods.isCommandDown());
            lanes->repaint();
            break;

        case Gesture::Kind::zoom:
            rubberBand = juce::Rectangle<int> (gesture->startPosition, e.getPosition()).withY (0).withHeight (lanes->getHeight());
            lanes->repaint();
            break;

        case Gesture::Kind::pending:
        default:
            break;
    }
}

void Playlist::beginDrag (const juce::MouseEvent& e)
{
    auto* ed = edit.get();
    auto* clip = gesture->clip.get();
    gesture->clickOnMouseUp = false;

    if (tool == Tool::select)
    {
        gesture->kind = Gesture::Kind::select;

        if (e.mods.isCommandDown())
            for (auto* item : selection.getSelectedObjects())
                gesture->selectedBefore.emplace_back (item);

        return;
    }

    if (tool == Tool::zoom)
    {
        gesture->kind = Gesture::Kind::zoom;
        return;
    }

    if (gesture->crossfade.has_value())
    {
        gesture->kind = Gesture::Kind::crossfade;
        ed->getUndoManager().beginNewTransaction ("Crossfade");
        return;
    }

    if (clip == nullptr || ! gesture->zone.has_value() || *gesture->zone == ClipZone::selectRange)
    {
        // The range is the selection now. (Nothing else stays selected, so Delete can't take a whole track
        // when a range was meant.)
        gesture->kind = Gesture::Kind::selectRange;
        selection.deselectAll();
        return;
    }

    auto& undo = ed->getUndoManager();

    switch (*gesture->zone)
    {
        case ClipZone::trimStart:
            gesture->kind = Gesture::Kind::trimStart;
            undo.beginNewTransaction ("Trim Clip Start");
            return;

        case ClipZone::trimEnd:
            gesture->kind = Gesture::Kind::trimEnd;
            undo.beginNewTransaction ("Trim Clip End");
            return;

        case ClipZone::fadeIn:
            gesture->kind = Gesture::Kind::fadeIn;
            undo.beginNewTransaction ("Fade In");
            return;

        case ClipZone::fadeOut:
            gesture->kind = Gesture::Kind::fadeOut;
            undo.beginNewTransaction ("Fade Out");
            return;

        case ClipZone::move:
        case ClipZone::selectRange:
        default:
            break;
    }

    // Moving: the clip under the mouse goes with the rest of the selection. With Ctrl, copies of them
    // move instead, and with Ctrl and Shift, aliases (copies that stay linked to their originals).
    gesture->kind = Gesture::Kind::move;

    if (! selection.isSelected (clip))
        selection.addToSelection (clip);

    auto clips = getSelectedClips (selection);
    const auto alias = e.mods.isCommandDown() && e.mods.isShiftDown();
    const auto copy = e.mods.isCommandDown() && ! alias;
    const auto many = clips.size() > 1;
    undo.beginNewTransaction (alias  ? (many ? "Make Aliases" : "Make Alias")
                              : copy ? (many ? "Copy Clips" : "Copy Clip")
                                     : (many ? "Move Clips" : "Move Clip"));

    if (copy || alias)
    {
        const auto anchorIndex = clips.indexOf (clip);
        clips = copyClips (clips, alias);
        clip = clips[anchorIndex];
        gesture->clip = clip;

        selection.deselectAll(); // the copies are what moves

        for (auto* c : clips)
            selection.addToSelection (c);
    }

    gesture->anchorStart = clip != nullptr ? clip->getStartBeat() : gesture->startBeat;

    for (auto* c : clips)
        gesture->moving.push_back ({ c, c->getStartBeat(), getRowOf (c->getTrack()) });
}

void Playlist::lanesMouseUp (const juce::MouseEvent& e)
{
    if (gesture == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        // A click, not a drag.
        if (tool == Tool::zoom)
        {
            zoomAround (e.position.x, e.mods.isAltDown() ? 0.5 : 2.0);
        }
        else if (auto* clip = gesture->clip.get(); clip != nullptr && gesture->clickOnMouseUp)
        {
            clipClicked (*clip, e.mods);
        }
        else if (tool == Tool::select && ! e.mods.isCommandDown())
        {
            range.clear();
            selection.deselectAll();
        }
    }
    else if (gesture->kind == Gesture::Kind::zoom && rubberBand.getWidth() > dragThreshold)
    {
        // Zoom to the dragged time.
        const auto from = view.xToBeat (rubberBand.getX()), to = view.xToBeat (rubberBand.getRight());
        view.pixelsPerBeat =
            juce::jlimit (TimelineView::minPixelsPerBeat, TimelineView::maxPixelsPerBeat, lanes->getWidth() / (to - from).inBeats());
        setViewStart (from);
        updateGridControls();
    }

    gesture = nullptr;

    if (! rubberBand.isEmpty())
    {
        rubberBand = {};
        lanes->repaint();
    }

    lanesMouseMove (e);
}

void Playlist::lanesDoubleClick (const juce::MouseEvent& e)
{
    auto* ed = edit.get();
    const auto row = getRowAt (e.y);

    if (ed == nullptr || tool != Tool::smart || row < 0 || getClipComponentAt (e.getPosition()) != nullptr)
        return;

    // On a track with an instrument: an empty MIDI clip, a bar long, from the grid line before the click.
    auto* track = dynamic_cast<te::AudioTrack*> (rows[(size_t) row].track.get());

    if (track == nullptr || ! hasInstrument (*track))
        return;

    const auto& tempo = ed->tempoSequence;
    auto start = view.xToBeat (e.position.x);

    if (grid.snap && ! e.mods.isAltDown())
        start = tempo.toBeats (getSnapType().roundTimeDown (tempo.toTime (start), tempo, grid.triplets));

    const auto bar = te::BeatDuration::fromBeats (tempo.getTimeSigAt (start).numerator.get());
    ed->getUndoManager().beginNewTransaction ("Add MIDI Clip");
    insertMidiClip (*track, { start, start + bar }, &selection);
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
    layOutClips();
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

    // The tools, with FL Studio's letters in the keymaps (Esc for the Smart Tool).
    for (const auto toolToAdd : allTools)
    {
        juce::StringArray synonyms { "tool", "pointer", "mode" };

        switch (toolToAdd)
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

        registry.add ({ .id = getToolCommandID (toolToAdd),
                        .name = toolToAdd == Tool::smart ? getToolName (toolToAdd) : getToolName (toolToAdd) + " Tool",
                        .category = "Tool",
                        .description = describeTool (toolToAdd),
                        .synonyms = synonyms,
                        .perform = [this, toolToAdd] { setTool (toolToAdd); },
                        .isTicked = [this, toolToAdd] { return tool == toolToAdd; } });
    }

    const auto canNudge = [this] { return ! range.isEmpty() || ! getSelectedClips (selection).isEmpty(); };
    const auto getUndoTarget = [this] { return edit.get(); };

    registry.add ({ .id = "clip.nudgeLeft",
                    .name = "Nudge Left",
                    .category = "Clip",
                    .description = "Moves the selected clips, or the selected time range, a step of the grid earlier.",
                    .synonyms = { "move left", "earlier", "shift" },
                    .perform = [this] { nudge (-1); },
                    .isEnabled = canNudge,
                    .undoTarget = getUndoTarget });

    registry.add ({ .id = "clip.nudgeRight",
                    .name = "Nudge Right",
                    .category = "Clip",
                    .description = "Moves the selected clips, or the selected time range, a step of the grid later.",
                    .synonyms = { "move right", "later", "shift" },
                    .perform = [this] { nudge (1); },
                    .isEnabled = canNudge,
                    .undoTarget = getUndoTarget });

    registry.add ({ .id = "clip.quantize",
                    .name = "Quantize Clip Starts",
                    .category = "Clip",
                    .description = "Moves the start of each selected clip to the nearest line of the grid.",
                    .synonyms = { "snap", "align", "grid" },
                    .perform = [this] { quantizeSelectedClips(); },
                    .isEnabled = [this] { return ! getSelectedClips (selection).isEmpty(); },
                    .undoTarget = getUndoTarget });

    refreshShortcutTexts();
}

void Playlist::refreshShortcutTexts()
{
    addTrackButton.setTooltip (withShortcut (commands, "Adds an empty track below the selected one", "track.add"));
    snapButton.setTooltip (
        withShortcut (commands, "Snap to grid: what you move or place lines up with the grid. Hold Alt to place freely", "grid.snap"));
    gridButton.setTooltip ("The grid's size. An adaptive grid gets finer as you zoom in");

    for (size_t i = 0; i < std::size (allTools); ++i)
        toolButtons[(int) i]->setTooltip (
            withShortcut (commands, toolButtons[(int) i]->getTitle() + ": " + describeTool (allTools[i]), getToolCommandID (allTools[i])));
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
    toolbar.removeFromLeft (Space::l);

    for (auto* button : toolButtons)
        button->setBounds (toolbar.removeFromLeft (controlHeight + Space::s));

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
void Playlist::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &range)
    {
        lanes->repaint();
        return;
    }

    updateHeaders();
    updateClips();
    lanes->repaint();

    // The clip selected last is the one the Draw and Paint tools place.
    if (auto* clip = selection.getFirstItemOfType<te::Clip>())
        lastClip = clip;

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
        layOutClips(); // clips are placed in time, and the timeline is in beats
        ruler->repaint();
        lanes->repaint();
    }
    else if (const auto clipState = findClipState (tree); clipState.isValid())
    {
        clipChanged (clipState); // moved, trimmed or faded, or its notes changed
    }
}

void Playlist::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (isTrackState (child) || te::Clip::isClipState (child))
        triggerAsyncUpdate();
    else if (const auto clipState = findClipState (parent); clipState.isValid())
        clipChanged (clipState); // a note, say
}

void Playlist::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (isTrackState (child) || te::Clip::isClipState (child))
        triggerAsyncUpdate();
    else if (const auto clipState = findClipState (parent); clipState.isValid())
        clipChanged (clipState);
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
