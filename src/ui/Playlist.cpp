// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Playlist.h"
#include "CommandUI.h"
#include "LookAndFeel.h"
#include "model/Clips.h"
#include "model/Import.h"
#include "model/Library.h"
#include "model/LiveInput.h"
#include "model/Mixer.h"
#include "model/Outlines.h"
#include "model/Playback.h"
#include "model/Sections.h"
#include "model/Tracks.h"

namespace phazite::ui
{
namespace
{
    constexpr int headerWidth = 200;
    constexpr int dragThreshold = 4;     // pixels the mouse moves before a click becomes a drag
    constexpr int focusedOutHeight = 20; // in focus mode, the tracks out of focus are this tall
    constexpr double scrollPerNotch = 48.0;

    const juce::String emptySpaceHint ("Click to select the track, drag to select a time range, and double-click on a track with an "
                                       "instrument to add a MIDI clip, or under the last track to add a track. Ctrl+wheel zooms; "
                                       "Alt+wheel resizes the tracks.");

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

    // Whether a dropped file can go on a track. Audio and notes don't share a track in v1 (docs/design.md,
    // section 5): an instrument would silence the audio.
    bool suitsTrack (te::AudioTrack& track, FileKind kind)
    {
        for (auto* clip : track.getClips())
            if ((dynamic_cast<te::MidiClip*> (clip) != nullptr) != (kind == FileKind::midi))
                return false;

        return kind == FileKind::midi || ! hasInstrument (track);
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

    // A clip on a track; sections are arranger clips, which the sections strip shows.
    bool isContentClipState (const juce::ValueTree& state)
    {
        return te::Clip::isClipState (state) && ! state.hasType (te::IDs::ARRANGERCLIP);
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
} // namespace

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

        auto* ed = playlist.getEdit();

        if (ed == nullptr)
            return;

        // The song's sections, as ghosts behind the tracks: a faint wash of each one's color.
        const auto sections = getSections (*ed);
        const auto& timeline = playlist.getView();
        const auto xOf = [&timeline] (te::BeatPosition beat) { return juce::roundToInt (timeline.beatToX (beat)); };

        for (auto* section : sections)
        {
            const auto left = xOf (section->getStartBeat());
            const auto wash =
                juce::Rectangle<int> (left, clip.getY(), xOf (section->getEndBeat()) - left, clip.getHeight()).getIntersection (clip);

            if (! wash.isEmpty())
            {
                g.setColour (getSectionColour (*section).withAlpha (playlist.selection.isSelected (section) ? SectionTint::selectedWash
                                                                                                            : SectionTint::wash));
                g.fillRect (wash);
            }
        }

        // Each row in its track's color, as the track's header is: more of it for folders, and for the
        // selected tracks.
        for (const auto& row : playlist.rows)
        {
            auto* track = row.track.get();
            const auto area = getRowArea (row).getIntersection (clip);

            if (track == nullptr || area.isEmpty())
                continue;

            const auto tint = playlist.selection.isSelected (track) ? TrackTint::selectedRow
                              : track->isFolderTrack()              ? TrackTint::folderRow
                                                                    : TrackTint::row;
            g.setColour (row.colour.withAlpha (tint));
            g.fillRect (area);
        }

        paintGridLines (g, playlist, clip);

        // A line under each track, and a stronger one under the pinned band.
        g.setColour (p.border);

        for (const auto& row : playlist.rows)
            g.fillRect (getRowArea (row).removeFromBottom (1).getIntersection (clip));

        if (playlist.pinnedHeight > 0)
        {
            g.setColour (p.textMuted);
            g.fillRect (clip.getX(), playlist.pinnedHeight - 2, clip.getWidth(), 2);
        }

        // Where each section starts, and where the last one ends: lines in their colors, over the grid.
        for (auto* section : sections)
        {
            g.setColour (getSectionColour (*section).withAlpha (SectionTint::edge));
            g.fillRect (xOf (section->getStartBeat()), clip.getY(), 1, clip.getHeight());
        }

        if (auto* last = sections.getLast())
        {
            g.setColour (getSectionColour (*last).withAlpha (SectionTint::edge));
            g.fillRect (xOf (last->getEndBeat()), clip.getY(), 1, clip.getHeight());
        }
    }

    // Where a row is in the lanes, cut to the part that shows: scrolling rows don't show in the pinned band.
    juce::Rectangle<int> getRowArea (const Row& row) const
    {
        const juce::Rectangle<int> rowArea (0, playlist.getRowTop (row), getWidth(), row.height);
        const auto band = getLocalBounds().withHeight (playlist.pinnedHeight);
        return rowArea.getIntersection (row.pinned ? band : getLocalBounds().withTrimmedTop (playlist.pinnedHeight));
    }

    // Over the clips: the time range selected across tracks, and the Select or Zoom tool's rectangle.
    void paintOverChildren (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto& timeline = playlist.getView();

        // The range, on each of its tracks' rows.
        if (! playlist.range.isEmpty())
        {
            const auto left = juce::roundToInt (timeline.beatToX (playlist.range.getRange().getStart()));
            const auto right = juce::roundToInt (timeline.beatToX (playlist.range.getRange().getEnd()));

            for (auto* track : playlist.range.getTracks())
            {
                if (const auto row = playlist.getRowOf (track); row >= 0)
                {
                    const auto area = getRowArea (playlist.rows[(size_t) row]).withX (left).withWidth (std::max (1, right - left));
                    g.setColour (p.accent.withAlpha (0.25f));
                    g.fillRect (area);
                    g.setColour (p.accent);
                    g.fillRect (area.withWidth (1));
                    g.fillRect (area.withTrimmedLeft (area.getWidth() - 1));
                }
            }
        }

        // Where dragged files would go: a line at that time, on that track or below the tracks.
        if (playlist.fileDropBeat.has_value())
        {
            const auto x = juce::roundToInt (timeline.beatToX (*playlist.fileDropBeat));
            const auto area = juce::isPositiveAndBelow (playlist.fileDropRow, (int) playlist.rows.size())
                                  ? getRowArea (playlist.rows[(size_t) playlist.fileDropRow])
                                  : getLocalBounds();
            g.setColour (p.accent);
            g.fillRect (x - 1, area.getY(), 3, area.getHeight());
        }

        // Where a device dragged from the browser would go: onto a track, or onto a new track under the last.
        if (playlist.deviceDropping)
        {
            auto lastRowBottom = 0;

            for (const auto& row : playlist.rows)
                lastRowBottom = std::max (lastRowBottom, getRowArea (row).getBottom());

            const auto area = juce::isPositiveAndBelow (playlist.fileDropRow, (int) playlist.rows.size())
                                  ? getRowArea (playlist.rows[(size_t) playlist.fileDropRow])
                                  : getLocalBounds().withTop (lastRowBottom);
            g.setColour (p.accent);
            g.drawRect (area, 2);
        }

        if (! playlist.rubberBand.isEmpty())
        {
            g.setColour (p.accent.withAlpha (0.15f));
            g.fillRect (playlist.rubberBand);
            g.setColour (p.accent);
            g.drawRect (playlist.rubberBand, 1);
        }

        // Where a section dragged in the time bar would go, or where its end would be.
        if (const auto target = playlist.sectionsStrip.getDragTarget())
        {
            g.setColour (p.accent);
            g.fillRect (juce::roundToInt (timeline.beatToX (*target)) - 1, 0, 3, getHeight());
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
// After the last track's header: a row that adds a track at the end, as the space after the tracks does in
// Logic, Cubase and REAPER. Right-clicking it offers a folder or a return track too.
struct Playlist::AddTrackRow : public juce::Button
{
    explicit AddTrackRow (Playlist& p)
        : juce::Button ("Add track at the end"),
          playlist (p)
    {
        setTitle ("Add track at the end");
        setDescription ("Click to add a track at the end, or right-click for a folder or a return track. Double-clicking under "
                        "the last track adds one too, and so does dropping an audio or MIDI file there.");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        onClick = [this] { playlist.addTrackAtEnd (false); };
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        // A dashed outline, as a place for a track to be.
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().reduced (Space::s, Space::s).toFloat();
        g.setColour (highlighted || down ? p.text : p.textMuted);

        juce::Path outline, dashed;
        outline.addRoundedRectangle (area, cornerRadius);
        const float dashes[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.fillPath (dashed);

        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText ("+ Add track", area, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            playlist.showAddTrackMenu();
        else
            juce::Button::mouseDown (e);
    }

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

    // Which ways the lanes scroll when this drags past their edges, if at all.
    std::optional<TimelineEditor::ScrollDirections> getAutoScrollDirections() const
    {
        switch (kind)
        {
            case Kind::move:
            case Kind::selectRange:
            case Kind::mute:
            case Kind::erase:
            case Kind::select:
                return TimelineEditor::ScrollDirections::both;
            case Kind::trimStart:
            case Kind::trimEnd:
            case Kind::paint:
            case Kind::zoom:
                return TimelineEditor::ScrollDirections::sideways;
            case Kind::pending:
            case Kind::fadeIn:
            case Kind::fadeOut:
            case Kind::crossfade:
            case Kind::slip:
            default:
                return std::nullopt;
        }
    }

    Kind kind = Kind::pending;
    te::SafeSelectable<te::Clip> clip; // the clip the mouse went down on, if any
    std::optional<ClipZone> zone;      // and where on it
    te::BeatPosition startBeat;        // the beat under the mouse when it went down
    int startRow = -1;
    int startScrollY = 0;        // how far the rows had scrolled then
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
Playlist::Playlist (CommandRegistry& registry, te::SelectionManager& selectionManager, RangeSelection& rangeSelection)
    : TimelineEditor (registry, {}),
      selection (selectionManager),
      range (rangeSelection),
      addTrackRow (std::make_unique<AddTrackRow> (*this)),
      lanes (std::make_unique<Lanes> (*this))
{
    setTitle ("Playlist");

    addTrackButton.setButtonText ("Track");
    addTrackButton.onClick = [this] { getCommands().invoke ("track.add"); };
    addTrackButton.setEnabled (false); // until a project is open

    // While a section is dragged, where it would go shows across the tracks too.
    sectionsStrip.onDragChanged = [this] { lanes->repaint(); };

    headerColumn.setTitle ("Track headers");
    headerColumn.addAndMakeVisible (scrollingHeaders);
    headerColumn.addAndMakeVisible (pinnedHeaders);
    scrollingHeaders.addAndMakeVisible (headerContent);
    headerContent.addChildComponent (*addTrackRow); // shown while a project is open
    pinnedHeaders.setTitle ("Pinned tracks");

    // The clips' layers let the mouse through to the lanes, which handle it.
    for (auto* layer : { &scrollingClips, &pinnedClips })
    {
        layer->setInterceptsMouseClicks (false, false);
        lanes->addAndMakeVisible (layer);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &sectionsStrip, &headerColumn, lanes.get() })
        addAndMakeVisible (c);

    selection.addChangeListener (this);
    range.addChangeListener (this);
    toolChanged(); // the Smart Tool's pointer and hint
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
    if (newEdit == getEdit() && editState.isValid() == (newEdit != nullptr))
        return;

    editState.removeListener (this);
    editState = newEdit != nullptr ? newEdit->state : juce::ValueTree();
    editState.addListener (this);

    scrollY = 0;
    dropRow = -1;
    selectionAnchor = nullptr;
    lastClip = nullptr;
    gesture = nullptr;
    rubberBand = {};
    fileDropBeat.reset();
    headers.clear();
    clipComponents.clear();
    rows.clear();
    cancelPendingUpdate();

    setTimelineEdit (newEdit);
    sectionsStrip.setEdit (newEdit);
    addTrackButton.setEnabled (newEdit != nullptr);
    addTrackRow->setVisible (newEdit != nullptr);
    rebuild();
}

//==============================================================================
void Playlist::rebuild()
{
    auto* e = getEdit();
    const auto tracks = e != nullptr ? getPlaylistTracks (*e) : juce::Array<te::Track*>();

    // The headers of tracks that are still shown are kept, in the tracks' new order. Tracks in
    // collapsed folders aren't shown.
    juce::OwnedArray<TrackHeader> newHeaders;
    rows.clear();

    for (int i = 0; i < tracks.size(); ++i)
    {
        auto* track = tracks[i];

        if (isHiddenInCollapsedFolder (*track))
            continue;

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
        rows.push_back ({ track, 0, 0, getTrackColour (*track, i), track->getTrackDepth(), isPinned (*track) });
    }

    headers.swapWith (newHeaders); // newHeaders now holds the hidden or deleted tracks' headers, and deletes them

    rebuildClips();
    layOutRows();
    updateHeaders();
    updateClips();
    ruler.repaint();
}

void Playlist::layOutRows()
{
    // Pinned rows go in the band at the top; the rest follow each other below it.
    auto pinnedTop = 0, scrollingTop = 0;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto& row = rows[i];
        auto& top = row.pinned ? pinnedTop : scrollingTop;
        row.top = top;

        if (auto* track = row.track.get())
            row.height = isInFocus (*track) ? getShownTrackHeight (*track) : focusedOutHeight;

        auto* header = headers[(int) i];
        auto& parent = row.pinned ? pinnedHeaders : headerContent;

        if (header->getParentComponent() != &parent)
            parent.addAndMakeVisible (header);

        header->setBounds (0, top, headerWidth, row.height);
        top += row.height;
    }

    pinnedHeight = std::min (pinnedTop, lanes->getHeight() / 2); // the band leaves room for the rest
    contentHeight = scrollingTop;

    pinnedHeaders.setBounds (0, 0, headerWidth, pinnedHeight);
    scrollingHeaders.setBounds (0, pinnedHeight, headerWidth, std::max (0, headerColumn.getHeight() - pinnedHeight));
    headerContent.setSize (headerWidth, contentHeight + TrackHeight::standard); // the scrolling leaves room for the row that adds a track
    addTrackRow->setBounds (0, contentHeight, headerWidth, TrackHeight::standard);
    pinnedClips.setBounds (0, 0, lanes->getWidth(), pinnedHeight);
    scrollingClips.setBounds (0, pinnedHeight, lanes->getWidth(), std::max (0, lanes->getHeight() - pinnedHeight));

    setScrollY (scrollY); // keeps it in range, and moves the headers and clips with it
}

void Playlist::updateHeaders()
{
    for (int i = 0; i < headers.size(); ++i)
        if (auto* track = rows[(size_t) i].track.get())
            headers[i]->update (rows[(size_t) i].colour, selection.isSelected (track), rows[(size_t) i].depth);
}

int Playlist::getRowTop (const Row& row) const
{
    return row.pinned ? row.top : pinnedHeight + row.top - scrollY;
}

bool Playlist::isInFocus (te::Track& track) const
{
    if (! focusMode)
        return true;

    // The selected tracks, the tracks of the selected clips, and the folders they are in. With nothing
    // selected, everything.
    const auto tracks = getSelectedTracks (selection);
    const auto clips = getSelectedClips (selection);

    if (tracks.isEmpty() && clips.isEmpty())
        return true;

    for (auto* selected : tracks)
        if (selected == &track || selected->isAChildOf (track))
            return true;

    for (auto* clip : clips)
        if (auto* clipTrack = clip->getTrack(); clipTrack == &track || (clipTrack != nullptr && clipTrack->isAChildOf (track)))
            return true;

    return false;
}

int Playlist::getRowAt (int y) const
{
    // In the pinned band, only pinned rows; below it, only the others.
    const auto inBand = y < pinnedHeight;

    for (size_t i = 0; i < rows.size(); ++i)
        if (const auto top = getRowTop (rows[i]); rows[i].pinned == inBand && y >= top && y < top + rows[i].height)
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
    const auto maxScroll = std::max (0, contentHeight + TrackHeight::standard - (lanes->getHeight() - pinnedHeight));
    scrollY = juce::jlimit (0, maxScroll, newScrollY);
    headerContent.setTopLeftPosition (0, -scrollY);
    layOutClips();
    lanes->repaint();
    updateVerticalScrollBar();
}

void Playlist::scrollToShow (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()) || rows[(size_t) row].pinned)
        return; // pinned rows are always in view

    const auto& r = rows[(size_t) row];
    const auto visibleHeight = lanes->getHeight() - pinnedHeight;

    if (r.top < scrollY)
        setScrollY (r.top);
    else if (r.top + r.height > scrollY + visibleHeight)
        setScrollY (r.top + r.height - visibleHeight);
}

void Playlist::updateVerticalScrollBar()
{
    const auto visibleHeight = std::max (1, lanes->getHeight() - pinnedHeight);
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
                scrollingClips.addAndMakeVisible (component); // positionClip() moves it to the right layer
            }

            kept.add (component);
        }
    }

    clipComponents.swapWith (kept);
}

void Playlist::layOutClips()
{
    for (auto* component : clipComponents)
        positionClip (*component);
}

void Playlist::positionClip (ClipComponent& component)
{
    auto* clip = component.getClip();
    const auto row = clip != nullptr ? getRowOf (clip->getTrack()) : -1;
    component.setVisible (row >= 0);

    if (row < 0)
        return;

    // A pinned track's clips are in the pinned layer, the others in the scrolling one, which keeps them
    // out of the pinned band.
    auto& layer = rows[(size_t) row].pinned ? pinnedClips : scrollingClips;

    if (component.getParentComponent() != &layer)
        layer.addAndMakeVisible (component);

    component.setBounds (getClipBounds (*clip) - layer.getPosition());
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
    const auto& timeline = getView();
    const auto left = juce::roundToInt (timeline.beatToX (clip.getStartBeat()));
    const auto right = juce::roundToInt (timeline.beatToX (clip.getEndBeat()));
    return { left, getRowTop (r) + 1, std::max (2, right - left), std::max (1, r.height - 3) };
}

ClipComponent* Playlist::getClipComponentAt (juce::Point<int> position) const
{
    // In the pinned band, only the pinned tracks' clips. The last one added is drawn on top, so it is
    // the one that is clicked.
    const auto inBand = position.y < pinnedHeight;

    for (auto i = clipComponents.size(); --i >= 0;)
    {
        auto* component = clipComponents.getUnchecked (i);
        auto* clip = component->getClip();
        const auto row = clip != nullptr ? getRowOf (clip->getTrack()) : -1;

        if (row >= 0 && rows[(size_t) row].pinned == inBand && getClipBounds (*clip).contains (position))
            return component;
    }

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
            positionClip (*component);
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
    auto& registry = getCommands();
    juce::PopupMenu menu;
    addCommandItems (menu, registry, { "edit.split", "edit.duplicate", "edit.duplicateAsAlias", "clip.makeUnique" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "edit.mute", "edit.quantize" });

    juce::PopupMenu fadeShapes;
    addCommandItems (fadeShapes, registry, { "clip.fadeLinear", "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve" });
    menu.addSubMenu ("Fade Shape", fadeShapes);

    menu.addSeparator();
    addCommandItems (menu, registry, { "edit.delete", "edit.deleteAndCloseGap" });
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
    auto* e = getEdit();
    const auto row = getRowAt (position.y);

    if (e == nullptr || row < 0)
        return {};

    // Only the lower third of a row, so the upper part still trims.
    const auto& r = rows[(size_t) row];
    auto* track = dynamic_cast<te::ClipTrack*> (r.track.get());

    if (track == nullptr || position.y < getRowTop (r) + r.height * 2 / 3)
        return {};

    const auto xOf = [this, e] (te::TimePosition time) { return getView().beatToX (e->tempoSequence.toBeats (time)); };

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
    auto* ed = getEdit();
    auto* track =
        juce::isPositiveAndBelow (row, (int) rows.size()) ? dynamic_cast<te::ClipTrack*> (rows[(size_t) row].track.get()) : nullptr;

    if (ed == nullptr || track == nullptr)
        return nullptr;

    // From the grid line at or before the beat.
    const auto start = snapStart ? snapDown (beat, {}) : beat;

    // A copy of the last clip used: of a MIDI clip, an alias, as FL Studio places the same pattern again.
    if (auto* source = lastClip.get();
        source != nullptr && te::containsClip (*ed, source) && dynamic_cast<te::AudioTrack*> (source->getTrack()) != nullptr)
    {
        const auto copies = copyClips ({ source }, dynamic_cast<te::MidiClip*> (source) != nullptr);

        if (copies.isEmpty())
            return nullptr;

        moveClip (*copies.getFirst(), start, *track);
        return copies.getFirst();
    }

    // No clip used yet: an empty MIDI clip, a bar long, on a track with an instrument.
    if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track); audioTrack != nullptr && hasInstrument (*audioTrack))
        return insertMidiClip (
            *track, { start, start + te::BeatDuration::fromBeats (ed->tempoSequence.getTimeSigAt (start).numerator.get()) }, nullptr);

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
        if (auto* clip = component->getClip(); clip != nullptr && getClipBounds (*clip).intersects (area))
            items.addIfNotAlreadyThere (clip);

    range.clear();
    selection.deselectAll();

    for (auto* item : items)
        selection.addToSelection (item);
}

//==============================================================================
bool Playlist::canNudgeSelection() const
{
    return ! range.isEmpty() || ! getSelectedClips (selection).isEmpty();
}

void Playlist::nudgeSelection (int direction)
{
    // A step of the grid as it is now, so what is seen is what moves.
    const auto step = te::BeatDuration::fromBeats (direction * getGridStepInBeats());

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

bool Playlist::canQuantizeSelection() const
{
    return ! getSelectedClips (selection).isEmpty();
}

void Playlist::quantizeSelection()
{
    auto* e = getEdit();

    if (e == nullptr)
        return;

    // To the grid as it is now, whether or not snapping is on: quantizing is asked for explicitly.
    for (auto* clip : getSelectedClips (selection))
        if (auto* track = clip->getClipTrack())
            moveClip (*clip, snapToGrid (clip->getStartBeat(), getSnapType(), e->tempoSequence, getGrid().triplets), *track);
}

//==============================================================================
te::BeatPosition Playlist::getFileDropBeat (int x) const
{
    const auto beat = te::BeatPosition::fromBeats (std::max (0.0, getView().xToBeat (x).inBeats()));
    return snapDown (beat, juce::ModifierKeys::currentModifiers);
}

void Playlist::importFiles (const juce::StringArray& paths, juce::Point<int> position)
{
    const auto row = getRowAt (position.y);
    addFiles (paths, row >= 0 ? rows[(size_t) row].track.get() : nullptr, getFileDropBeat (position.x));
}

bool Playlist::placeFile (const juce::File& file)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    return addFiles ({ file.getFullPathName() }, getSelectedTracks (selection).getFirst(), e->tempoSequence.toBeats (getPlayStart (*e)));
}

bool Playlist::addFiles (const juce::StringArray& paths, te::Track* target, te::BeatPosition start)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    // The first file can go on the target track; the others get new tracks below it.
    auto* after = target != nullptr ? target : getPlaylistTracks (*e).getLast();
    juce::Array<te::Clip*> added;

    e->getUndoManager().beginNewTransaction (paths.size() > 1 ? "Add Files" : "Add File");

    for (const auto& path : paths)
    {
        const juce::File file (path);
        const auto kind = getFileKind (e->engine, file);

        if (kind == FileKind::other)
            continue;

        // A new track is named after the file, and colored by what its name says it is.
        auto* track = dynamic_cast<te::AudioTrack*> (target);

        if (track == nullptr || ! suitsTrack (*track, kind))
        {
            track = insertTrack (*e, after, nullptr);

            if (track == nullptr)
                break;

            track->setName (file.getFileNameWithoutExtension());

            if (const auto colour = getColourForName (file.getFileNameWithoutExtension()))
                track->setColour (*colour);
        }

        if (auto* clip = importFile (*track, file, start))
            added.add (clip);

        after = track;
        target = nullptr;
    }

    range.clear();
    selection.deselectAll();

    for (auto* clip : added)
        selection.addToSelection (clip);

    if (added.isEmpty())
        return false;

    lastClip = added.getFirst(); // what Draw and Paint place next
    return true;
}

bool Playlist::isInterestedInFileDrag (const juce::StringArray& paths)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    return std::any_of (paths.begin(), paths.end(),
                        [e] (const juce::String& path) { return getFileKind (e->engine, juce::File (path)) != FileKind::other; });
}

void Playlist::fileDragMove (const juce::StringArray&, int x, int y)
{
    // Where importFiles() would put them.
    const auto position = lanes->getLocalPoint (this, juce::Point<int> (x, y));
    fileDropRow = getRowAt (position.y);
    fileDropBeat = getFileDropBeat (position.x);
    lanes->repaint();
}

void Playlist::fileDragExit (const juce::StringArray&)
{
    fileDropBeat.reset();
    lanes->repaint();
}

void Playlist::filesDropped (const juce::StringArray& paths, int x, int y)
{
    fileDropBeat.reset();
    lanes->repaint();
    importFiles (paths, lanes->getLocalPoint (this, juce::Point<int> (x, y)));
}

bool Playlist::isInterestedInDragSource (const SourceDetails& details)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    if (const auto file = getDraggedFile (details.description))
        return getFileKind (e->engine, *file) != FileKind::other;

    return getDraggedDevice (details.description).has_value();
}

void Playlist::itemDragMove (const SourceDetails& details)
{
    // Where it would go, as for files from Explorer: a file at a beat, a device onto a track.
    const auto position = lanes->getLocalPoint (this, details.localPosition);
    fileDropRow = getRowAt (position.y);
    deviceDropping = getDraggedDevice (details.description).has_value();

    if (deviceDropping)
        fileDropBeat.reset();
    else
        fileDropBeat = getFileDropBeat (std::max (0, position.x));

    lanes->repaint();
}

void Playlist::itemDragExit (const SourceDetails&)
{
    fileDropBeat.reset();
    deviceDropping = false;
    lanes->repaint();
}

void Playlist::itemDropped (const SourceDetails& details)
{
    itemDragExit (details);
    auto* e = getEdit();

    if (e == nullptr)
        return;

    const auto position = lanes->getLocalPoint (this, details.localPosition);
    const auto row = getRowAt (position.y);
    auto* track = row >= 0 ? rows[(size_t) row].track.get() : nullptr;
    auto placed = false;

    if (const auto file = getDraggedFile (details.description))
        placed = addFiles ({ file->getFullPathName() }, track, getFileDropBeat (std::max (0, position.x)));
    else if (const auto device = getDraggedDevice (details.description))
        placed = addDeviceTo (*e, track, *device, &selection) != nullptr;

    if (placed)
        markPlaced (details.description);
}

//==============================================================================
void Playlist::toolChanged()
{
    gesture = nullptr;
    rubberBand = {};

    lanes->setMouseCursor (getCursorFor (getTool()));
    lanes->setDescription (getTool() == Tool::smart ? emptySpaceHint : describeTool (getTool()));
    lanes->repaint();
}

void Playlist::lanesMouseMove (const juce::MouseEvent& e)
{
    const auto currentTool = getTool();

    // The hint bar and the pointer say what the mouse does here. With the Smart Tool, and with Draw over a
    // clip, that depends on where on the clip it is.
    if (currentTool == Tool::smart && findCrossfadeAt (e.getPosition()).has_value())
    {
        lanes->setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        lanes->setDescription ("Drag to crossfade the two clips: each plays over the other while one fades out and the other in.");
        return;
    }

    auto* component = getClipComponentAt (e.getPosition());

    if (component != nullptr && (currentTool == Tool::smart || currentTool == Tool::draw))
    {
        const auto zone = getClipZone (getClipBounds (*component->getClip()).toFloat(), e.position,
                                       dynamic_cast<te::AudioClipBase*> (component->getClip()) != nullptr);
        lanes->setMouseCursor (getCursorFor (zone));
        lanes->setDescription (describeClipZone (zone));
        return;
    }

    lanes->setMouseCursor (getCursorFor (currentTool));
    lanes->setDescription (currentTool == Tool::smart ? emptySpaceHint : describeTool (currentTool));
}

void Playlist::lanesMouseDown (const juce::MouseEvent& e)
{
    gesture = nullptr;
    auto* ed = getEdit();

    if (ed == nullptr)
        return;

    gesture = std::make_unique<Gesture>();
    gesture->startPosition = e.getPosition();
    gesture->startBeat = getView().xToBeat (e.position.x);
    gesture->startRow = getRowAt (e.y);
    gesture->startScrollY = scrollY;
    beginDragAutoRepeat (autoRepeatInterval); // so the lanes go on scrolling while the mouse rests past an edge

    auto* component = getClipComponentAt (e.getPosition());
    auto* clip = component != nullptr ? component->getClip() : nullptr;
    auto& undo = ed->getUndoManager();
    auto currentTool = getTool();

    if (e.mods.isPopupMenu())
    {
        // In FL Studio's keymap, right-click deletes, as in FL Studio: this clip and those the mouse moves on to.
        // Shift+right-click opens the menu, as in the piano roll.
        if (getCommands().getKeymap().rightClickDeletes && ! e.mods.isShiftDown())
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

    // With FL Studio's keymap, the Smart Tool places the last clip used where empty space is clicked, as FL's Draw
    // tool does. Ctrl+drag still selects a time range there.
    const auto placesForSmartTool = currentTool == Tool::smart && clip == nullptr && getCommands().getKeymap().clickPlacesClips
                                    && ! e.mods.isCommandDown() && ! findCrossfadeAt (e.getPosition()).has_value();

    if (placesForSmartTool)
        currentTool = Tool::draw;

    switch (currentTool)
    {
        case Tool::draw:
        case Tool::paint:
        {
            // Draw drags clips as the Smart Tool does; Paint selects them.
            if (clip != nullptr)
            {
                if (currentTool == Tool::draw)
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
            undo.beginNewTransaction (currentTool == Tool::draw ? "Place Clip" : "Paint Clips");
            auto* placed = placeClip (gesture->startRow, gesture->startBeat, getGrid().snap && ! e.mods.isAltDown());

            if (placed == nullptr)
            {
                // Nothing to place (a track without an instrument, before any clip was used): the Smart Tool's click.
                if (placesForSmartTool)
                {
                    smartToolDown (e);
                    return;
                }

                gesture = nullptr;
                return;
            }

            selection.selectOnly (placed);

            if (currentTool == Tool::draw)
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
            undo.beginNewTransaction (currentTool == Tool::mute ? "Mute Clips" : "Delete Clips");
            gesture->kind = currentTool == Tool::mute ? Gesture::Kind::mute : Gesture::Kind::erase;
            muteOrDeleteClipAt (e.getPosition(), currentTool == Tool::erase);
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
        gesture->zone = getClipZone (getClipBounds (clip).toFloat(), e.position, dynamic_cast<te::AudioClipBase*> (&clip) != nullptr);

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
    auto* ed = getEdit();

    if (gesture == nullptr || ed == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        if (e.getDistanceFromDragStart() < dragThreshold)
            return;

        beginDrag (e);
    }

    // Past an edge, the lanes scroll along, and the drag goes on from what is under the pointer then. Rows are
    // found where the pointer is, or at the edge it is past.
    if (const auto directions = gesture->getAutoScrollDirections())
        autoScroll (e.getPosition(), lanes->getLocalBounds().withTrimmedTop (pinnedHeight), *directions);

    const auto beat = getView().xToBeat (e.position.x);
    const auto rowAtPointer = getRowAt (juce::jlimit (0, lanes->getHeight() - 1, e.y));
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
            if (const auto row = rowAtPointer; row >= 0 && gesture->startRow >= 0)
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
            const auto row = rowAtPointer >= 0 ? rowAtPointer : lastRow; // below the last row: up to it

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
            rubberBand = juce::Rectangle<int> (getGestureStart(), e.getPosition());
            selectClipsIn (rubberBand, e.mods.isCommandDown());
            lanes->repaint();
            break;

        case Gesture::Kind::zoom:
            rubberBand = juce::Rectangle<int> (getGestureStart(), e.getPosition()).withY (0).withHeight (lanes->getHeight());
            lanes->repaint();
            break;

        case Gesture::Kind::pending:
        default:
            break;
    }
}

void Playlist::beginDrag (const juce::MouseEvent& e)
{
    auto* ed = getEdit();
    auto* clip = gesture->clip.get();
    gesture->clickOnMouseUp = false;

    if (getTool() == Tool::select)
    {
        gesture->kind = Gesture::Kind::select;

        if (e.mods.isCommandDown())
            for (auto* item : selection.getSelectedObjects())
                gesture->selectedBefore.emplace_back (item);

        return;
    }

    if (getTool() == Tool::zoom)
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

juce::Point<int> Playlist::getGestureStart() const
{
    // The lanes may have scrolled since; the pinned band doesn't scroll up and down.
    const auto& start = gesture->startPosition;
    const auto y = start.y < pinnedHeight ? start.y : start.y + gesture->startScrollY - scrollY;
    return { juce::roundToInt (getView().beatToX (gesture->startBeat)), y };
}

void Playlist::lanesMouseUp (const juce::MouseEvent& e)
{
    if (gesture == nullptr)
        return;

    if (gesture->kind == Gesture::Kind::pending)
    {
        // A click, not a drag.
        if (getTool() == Tool::zoom)
        {
            zoomAround (e.position.x, e.mods.isAltDown() ? 0.5 : 2.0);
        }
        else if (auto* clip = gesture->clip.get(); clip != nullptr && gesture->clickOnMouseUp)
        {
            clipClicked (*clip, e.mods);
        }
        else if (getTool() == Tool::select && ! e.mods.isCommandDown())
        {
            range.clear();
            selection.deselectAll();
        }
    }
    else if (gesture->kind == Gesture::Kind::zoom && rubberBand.getWidth() > dragThreshold)
    {
        // Zoom to the dragged time.
        zoomTo ({ getView().xToBeat (rubberBand.getX()), getView().xToBeat (rubberBand.getRight()) });
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
    auto* ed = getEdit();
    const auto row = getRowAt (e.y);

    if (ed == nullptr || getTool() != Tool::smart)
        return;

    // Under the last track: a new one at the end, as in Logic, Cubase and REAPER.
    if (row < 0)
    {
        if (e.y >= pinnedHeight + contentHeight - scrollY)
            addTrackAtEnd (false);

        return;
    }

    // On a MIDI clip: open it in the piano roll.
    if (auto* component = getClipComponentAt (e.getPosition()))
    {
        if (auto* midi = dynamic_cast<te::MidiClip*> (component->getClip()); midi != nullptr && onOpenClip != nullptr)
            onOpenClip (*midi);

        return;
    }

    // On a track with an instrument: an empty MIDI clip, a bar long, from the grid line before the click.
    auto* track = dynamic_cast<te::AudioTrack*> (rows[(size_t) row].track.get());

    if (track == nullptr || ! hasInstrument (*track))
        return;

    const auto start = snapDown (getView().xToBeat (e.position.x), e.mods);
    const auto bar = te::BeatDuration::fromBeats (ed->tempoSequence.getTimeSigAt (start).numerator.get());
    ed->getUndoManager().beginNewTransaction ("Add MIDI Clip");
    insertMidiClip (*track, { start, start + bar }, &selection);
}

//==============================================================================
juce::Rectangle<int> Playlist::getTimelineArea() const
{
    return lanes->getBounds();
}

te::BeatRange Playlist::getContentRange() const
{
    auto end = 16.0; // an empty song still shows some bars

    if (auto* e = getEdit())
        end = std::max (end, e->tempoSequence.toBeats (te::toPosition (e->getLength())).inBeats());

    return { te::BeatPosition(), te::BeatPosition::fromBeats (end) };
}

void Playlist::timelineChanged()
{
    layOutClips();
    lanes->repaint();
    sectionsStrip.repaint();
}

void Playlist::gridChanged()
{
    lanes->repaint();
}

void Playlist::verticalScrollBarMoved (double newRangeStart)
{
    setScrollY (juce::roundToInt (newRangeStart));
}

bool Playlist::scrollVerticallyBy (int pixels)
{
    const auto before = scrollY;
    setScrollY (scrollY + pixels);
    return scrollY != before;
}

void Playlist::addTrackAtEnd (bool folder)
{
    auto* e = getEdit();

    if (e == nullptr)
        return;

    e->getUndoManager().beginNewTransaction (folder ? "Add Folder" : "Add Track");
    te::Track* added = folder ? static_cast<te::Track*> (insertFolder (*e, nullptr, &selection)) : insertTrack (*e, nullptr, &selection);

    handleUpdateNowIfNeeded(); // its row is there now
    scrollToShow (getRowOf (added));
}

void Playlist::showAddTrackMenu()
{
    const juce::Component::SafePointer<Playlist> safeThis (this);
    juce::PopupMenu menu;
    menu.addSectionHeader ("Add at the end");

    menu.addItem ("Track",
                  [safeThis]
                  {
                      if (safeThis != nullptr)
                          safeThis->addTrackAtEnd (false);
                  });

    menu.addItem ("Folder",
                  [safeThis]
                  {
                      if (safeThis != nullptr)
                          safeThis->addTrackAtEnd (true);
                  });

    addCommandItem (menu, getCommands(), "track.addReturn"); // returns go at the end anyway
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::showRulerMenu (te::BeatPosition beat)
{
    auto* e = getEdit();

    if (e == nullptr)
        return;

    // As a right-click on the section there would.
    auto* section = getSectionAt (*e, beat);

    if (section != nullptr && ! selection.isSelected (section))
        selection.selectOnly (section);

    sectionsStrip.showMenu (section, beat);
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
    auto& registry = getCommands();
    juce::PopupMenu menu;
    addCommandItems (menu, registry, { "edit.rename", "track.color" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "track.mute", "track.solo", "track.pin" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "track.moveUp", "track.moveDown" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "track.groupIntoFolder", "track.ungroup", "track.collapse" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "edit.duplicate", "edit.delete" });
    menu.addSeparator();
    addCommandItems (menu, registry, { "track.add", "track.addFolder" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::showColourMenu (te::Track& track, juce::Component& target)
{
    ui::showColourMenu (target, getTrackColour (track, getRowOf (&track)),
                        [safeThis = juce::Component::SafePointer<Playlist> (this)] (juce::Colour colour)
                        {
                            if (safeThis != nullptr)
                                safeThis->setColourOfSelectedTracks (colour);
                        });
}

void Playlist::trackDragged (TrackHeader&, const juce::MouseEvent& e)
{
    // Past the top or bottom, the tracks scroll along.
    const auto position = e.getEventRelativeTo (lanes.get()).getPosition();
    beginDragAutoRepeat (autoRepeatInterval);
    autoScroll (position, lanes->getLocalBounds().withTrimmedTop (pinnedHeight), ScrollDirections::upAndDown);
    const auto y = position.y;

    // The track would go to the nearest boundary between rows, as they are on screen.
    const auto boundary = [this] (size_t i) { return i < rows.size() ? getRowTop (rows[i]) : pinnedHeight + contentHeight - scrollY; };
    size_t nearest = 0;

    for (size_t i = 1; i <= rows.size(); ++i)
        if (std::abs (y - boundary (i)) < std::abs (y - boundary (nearest)))
            nearest = i;

    dropRow = (int) nearest;
    repaint();
}

void Playlist::trackSelectionDragged (TrackHeader& header, const juce::MouseEvent& e)
{
    // From the track the drag started on to the one under the pointer, as a Shift+click would.
    const auto position = e.getEventRelativeTo (lanes.get()).getPosition();
    beginDragAutoRepeat (autoRepeatInterval);
    autoScroll (position, lanes->getLocalBounds().withTrimmedTop (pinnedHeight), ScrollDirections::upAndDown);

    const auto from = getRowOf (header.getTrack());
    auto to = getRowAt (juce::jlimit (0, lanes->getHeight() - 1, position.y));

    if (from < 0)
        return;

    if (to < 0) // below the last row
        to = (int) rows.size() - 1;

    selection.deselectAll();

    for (auto i = std::min (from, to); i <= std::max (from, to); ++i)
        if (auto* t = rows[(size_t) i].track.get())
            selection.addToSelection (t);

    selectionAnchor = header.getTrack();
}

void Playlist::trackDragEnded (TrackHeader&, const juce::MouseEvent&)
{
    const auto target = dropRow;
    dropRow = -1;
    repaint();

    auto* e = getEdit();
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
    const auto hasEdit = [this] { return getEdit() != nullptr; };
    const auto hasSelectedTracks = [this] { return ! getSelectedTracks (selection).isEmpty(); };
    const auto hasSelectedSections = [this] { return ! getSelectedSections (selection).isEmpty(); };

    registry.add ({ .id = "edit.rename",
                    .name = "Rename",
                    .category = "Edit",
                    .description = "Renames the selected section or track.",
                    .synonyms = { "name", "label", "title" },
                    .perform =
                        [this]
                    {
                        if (auto* section = getSelectedSections (selection).getFirst())
                        {
                            sectionsStrip.startRenaming (*section);
                            return;
                        }

                        handleUpdateNowIfNeeded(); // a track added a moment ago has its header now

                        if (auto* track = getSelectedTracks (selection).getFirst())
                        {
                            scrollToShow (getRowOf (track));

                            if (auto* header = getHeaderFor (track))
                                header->startRenaming();
                        }
                    },
                    .isEnabled = [hasSelectedTracks, hasSelectedSections] { return hasSelectedSections() || hasSelectedTracks(); },
                    .currentName =
                        [hasSelectedTracks, hasSelectedSections]
                    {
                        if (hasSelectedSections())
                            return juce::String ("Rename Section");

                        return hasSelectedTracks() ? juce::String ("Rename Track") : juce::String();
                    } });

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

    registry.add ({ .id = "view.focusMode",
                    .name = "Focus Mode",
                    .category = "View",
                    .description = "Makes every track small but the selected ones, and the folders they are in.",
                    .synonyms = { "solo view", "collapse others", "zoom tracks", "concentrate" },
                    .perform =
                        [this]
                    {
                        focusMode = ! focusMode;
                        layOutRows();
                    },
                    .isEnabled = hasEdit,
                    .isTicked = [this] { return focusMode; } });
}

void Playlist::refreshShortcutTexts()
{
    TimelineEditor::refreshShortcutTexts();
    addTrackButton.setTooltip (withShortcut (getCommands(), "Adds an empty track below the selected one", "track.add"));
}

//==============================================================================
void Playlist::paint (juce::Graphics& g)
{
    paintEditorBackground (g);
}

void Playlist::paintOverChildren (juce::Graphics& g)
{
    if (dropRow < 0)
        return;

    // A line where the dragged track would go.
    const auto y = dropRow < (int) rows.size() ? getRowTop (rows[(size_t) dropRow]) : pinnedHeight + contentHeight - scrollY;
    g.setColour (getPalette (*this).accent);
    g.fillRect (headerColumn.getX(), lanes->getY() + y - 1, lanes->getRight() - headerColumn.getX(), 3);
}

void Playlist::resized()
{
    auto area = getLocalBounds();
    layOutToolbarControls (area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs));

    horizontalScrollBar.setBounds (area.removeFromBottom (scrollBarSize).withTrimmedLeft (headerWidth).withTrimmedRight (scrollBarSize));

    // The time bar: the song's sections over the bar numbers. Left of it, over the track headers, the button
    // that adds a track.
    auto timeBar = area.removeFromTop (SectionsStrip::preferredHeight + rulerHeight).withTrimmedRight (scrollBarSize);
    addTrackButton.setBounds (timeBar.removeFromLeft (headerWidth).reduced (Space::s, Space::s).withWidth (96));
    sectionsStrip.setBounds (timeBar.removeFromTop (SectionsStrip::preferredHeight));
    ruler.setBounds (timeBar);

    verticalScrollBar.setBounds (area.removeFromRight (scrollBarSize));
    headerColumn.setBounds (area.removeFromLeft (headerWidth));
    lanes->setBounds (area);

    layOutRows();
    setViewStart (getView().start);
}

void Playlist::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Ctrl+wheel zooms around the pointer, as in FL Studio.
    if (e.mods.isCommandDown())
    {
        zoomByWheel (e.getEventRelativeTo (lanes.get()).position.x, wheel.deltaY);
        return;
    }

    // Alt+wheel makes every track taller or shorter.
    if (e.mods.isAltDown())
    {
        if (auto* ed = getEdit())
        {
            const auto change = juce::roundToInt (getWheelNotches (wheel.deltaY) * TrackHeight::step / 2);

            for (auto* track : getPlaylistTracks (*ed))
                resizeTrack (*track, change);

            layOutRows();
        }

        return;
    }

    const auto sideways = e.mods.isShiftDown() ? wheel.deltaY : wheel.deltaX;
    const auto upDown = e.mods.isShiftDown() ? 0.0f : wheel.deltaY;

    if (! juce::exactlyEqual (sideways, 0.0f))
        scrollByWheel (sideways);

    if (! juce::exactlyEqual (upDown, 0.0f))
        setScrollY (scrollY - juce::roundToInt (getWheelNotches (upDown) * scrollPerNotch));
}

//==============================================================================
void Playlist::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &range)
    {
        lanes->repaint();

        // With FL Studio's keymap, the selected time range loops while it is selected, and then the song again.
        if (auto* e = getEdit(); e != nullptr && getCommands().getKeymap().selectionLoops)
        {
            if (! range.isEmpty())
                loopRange (*e, e->tempoSequence.toTime (range.getRange()));
            else if (loopsSelection)
                loopSong (*e);

            loopsSelection = ! range.isEmpty();
        }

        return;
    }

    updateHeaders();
    updateClips();
    lanes->repaint();

    if (focusMode)
        layOutRows(); // what is in focus changed

    // The clip selected last is the one the Draw and Paint tools place (not a section, which is a clip too).
    if (auto* clip = getSelectedClips (selection).getFirst())
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
        ruler.repaint();
        lanes->repaint();
    }
    else if (const auto clipState = findClipState (tree); clipState.isValid())
    {
        clipChanged (clipState); // moved, trimmed or faded, or its notes changed
    }
}

void Playlist::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (isTrackState (child) || isContentClipState (child))
        triggerAsyncUpdate();
    else if (const auto clipState = findClipState (parent); clipState.isValid())
        clipChanged (clipState); // a note, say
}

void Playlist::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (isTrackState (child) || isContentClipState (child))
        triggerAsyncUpdate();
    else if (const auto clipState = findClipState (parent); clipState.isValid())
        clipChanged (clipState);
}

void Playlist::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == editState || isTrackState (parent))
        triggerAsyncUpdate();
}
} // namespace phazite::ui
