// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks shown as rows, and their clips on them.
#include "PlaylistParts.h"
#include "TrackHeights.h"

namespace phazite::ui
{
namespace
{
    constexpr int focusedOutHeight = 20; // in focus mode, the tracks out of focus are this tall
} // namespace

std::vector<TrackRow> Playlist::getShownRows() const
{
    auto shown = getPlaylistRows (context);

    if (auto master = getMasterRow (context))
        shown.push_back (std::move (*master));

    return shown;
}

void Playlist::rebuild()
{
    // The headers of tracks that are still shown are kept, in the tracks' new order. Tracks in collapsed folders
    // aren't shown. The master shows under them while it has automation lanes.
    juce::OwnedArray<TrackHeader> newHeaders;
    rows.clear();
    rowOf.clear();

    for (const auto& track : getShownRows())
    {
        auto* header = getHeaderFor (track.id);

        if (header != nullptr)
        {
            headers.removeObject (header, false);
        }
        else
        {
            header = new TrackHeader (*this, context, track);
            headerContent.addAndMakeVisible (header);
        }

        newHeaders.add (header);
        rowOf[track.id] = (int) rows.size();
        rows.push_back ({ track });

        for (const auto& lane : getAutomationLanes (context, track.id))
            rows.back().lanes.push_back ({ lane });
    }

    headers.swapWith (newHeaders); // newHeaders now holds the hidden or deleted tracks' headers, and deletes them

    rebuildLanes();
    rebuildClips();
    layOutRows();
    updateHeaders();
    updateLanes();
    updateClips();
    ruler.repaint();
    noteLanes();
}

void Playlist::layOutRows()
{
    // Pinned rows go in the band at the top; the rest follow each other below it.
    auto pinnedTop = 0, scrollingTop = 0;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto& row = rows[i];
        auto& top = row.track.pinned ? pinnedTop : scrollingTop;
        const auto inFocus = isInFocus (row.track.id);
        row.top = top;
        row.height = row.track.kind == TrackType::master ? TrackHeight::collapsed
                     : inFocus                           ? getShownTrackHeight (row.track)
                                                         : focusedOutHeight;

        // Its lanes under it, while it is in focus: thin rows while it is one, or each that is collapsed, but never hidden,
        // as their automation plays (focus mode is a view of some tracks only). A folder collapses by hiding its tracks.
        const auto collapsesLanes = row.track.collapsed && row.track.kind != TrackType::folder;
        row.lanesHeight = 0;

        for (auto& lane : row.lanes)
        {
            lane.collapsed = collapsesLanes || lane.lane.collapsed;
            lane.top = row.height + row.lanesHeight;
            lane.height = ! inFocus ? 0 : lane.collapsed ? TrackHeight::collapsed : lane.lane.height;
            row.lanesHeight += lane.height;
        }

        auto* header = headers[(int) i];
        auto& parent = row.track.pinned ? pinnedHeaders : headerContent;

        if (header->getParentComponent() != &parent)
            parent.addAndMakeVisible (header);

        header->setBounds (0, top, headerWidth, row.height);
        top += row.getTotalHeight();
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

void Playlist::trackHeightChanged()
{
    // The same tracks, taller or shorter or collapsed: the rows as the model keeps them now. (A folder collapsed or
    // opened shows other tracks: then the rows are built again.)
    const auto fresh = getShownRows();
    const auto same =
        fresh.size() == rows.size()
        && std::equal (fresh.begin(), fresh.end(), rows.begin(), [] (const TrackRow& a, const Row& b) { return a.id == b.track.id; });

    if (! same)
    {
        rebuild();
        return;
    }

    for (size_t i = 0; i < rows.size(); ++i)
        rows[i].track = fresh[i];

    refreshLanes(); // shown or hidden, taller or shorter; and lays out the rows
    updateHeaders();
}

void Playlist::updateHeaders()
{
    for (int i = 0; i < headers.size(); ++i)
        headers[i]->update (rows[(size_t) i].track, selection.isSelected (rows[(size_t) i].track.id));
}

int Playlist::getRowTop (const Row& row) const
{
    return row.track.pinned ? row.top : pinnedHeight + row.top - scrollY;
}

bool Playlist::isInFocus (TrackID track) const
{
    if (! focusMode)
        return true;

    // The selected tracks, the tracks of the selected clips, and the folders they are in. With nothing
    // selected, everything.
    const auto tracks = selection.getTracks();
    const auto clips = selection.getClips();

    if (tracks.empty() && clips.empty())
        return true;

    for (const auto selected : tracks)
        if (selected == track || isInside (context, selected, track))
            return true;

    for (const auto clipID : clips)
        if (const auto clip = getClip (context, clipID);
            clip.has_value() && (clip->track == track || isInside (context, clip->track, track)))
            return true;

    return false;
}

int Playlist::getRowAt (int y) const
{
    // In the pinned band, only pinned rows; below it, only the others.
    const auto inBand = y < pinnedHeight;

    for (size_t i = 0; i < rows.size(); ++i)
        if (const auto top = getRowTop (rows[i]); rows[i].track.pinned == inBand && y >= top && y < top + rows[i].getTotalHeight())
            return (int) i;

    return -1;
}

int Playlist::getRowOf (TrackID track) const
{
    const auto found = rowOf.find (track);
    return found != rowOf.end() ? found->second : -1;
}

TrackHeader* Playlist::getHeaderFor (TrackID track) const
{
    for (auto* header : headers)
        if (track.isValid() && header->getTrack() == track)
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
    layOutLanes();
    lanes->repaint();
    updateVerticalScrollBar();
}

void Playlist::scrollToShow (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()) || rows[(size_t) row].track.pinned)
        return; // pinned rows are always in view

    const auto& r = rows[(size_t) row];
    const auto visibleHeight = lanes->getHeight() - pinnedHeight;

    if (r.top < scrollY)
        setScrollY (r.top);
    else if (r.top + r.getTotalHeight() > scrollY + visibleHeight)
        setScrollY (r.top + r.getTotalHeight() - visibleHeight);
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
    // The clips on the rows as they are now. The components of those still there show them as they are now; those of clips
    // that went go, and so do their waveforms. layOutClips() makes the components of clips that come into view.
    shownClips.clear();
    clipIndex.clear();

    for (const auto& row : rows)
    {
        for (auto& clip : getClips (context, row.track.id))
        {
            clipIndex.emplace (clip.id, shownClips.size());
            shownClips.push_back (std::move (clip));
        }
    }

    std::erase_if (clipComponents, [this] (const auto& entry) { return ! clipIndex.contains (entry.first); });
    std::erase_if (waveforms, [this] (const auto& entry) { return ! clipIndex.contains (entry.first); });

    for (const auto& [id, component] : clipComponents)
        component->showClip (shownClips[clipIndex.at (id)]);
}

void Playlist::layOutClips()
{
    // A pinned track's clips are in the pinned layer, the others in the scrolling one, which keeps them out of the pinned
    // band; each layer's bounds are what of it shows. Components of clips that left the view go.
    std::unordered_map<ClipID, std::unique_ptr<ClipComponent>> inView;

    for (size_t i = 0; i < shownClips.size(); ++i)
    {
        const auto& clip = shownClips[i];
        const auto row = getRowOf (clip.track);

        if (row < 0)
            continue;

        auto& layer = rows[(size_t) row].track.pinned ? pinnedClips : scrollingClips;
        const auto bounds = getClipBounds (clip);

        if (! bounds.intersects (layer.getBounds()))
            continue;

        auto kept = clipComponents.extract (clip.id);
        auto& component = kept.empty() ? *inView.emplace (clip.id, makeClipComponent (clip)).first->second
                                       : *inView.insert (std::move (kept)).position->second;

        if (component.getParentComponent() != &layer)
            layer.addAndMakeVisible (component, getZOrderFor (layer, i));

        component.setBounds (bounds - layer.getPosition());
    }

    clipComponents = std::move (inView);
}

std::unique_ptr<ClipComponent> Playlist::makeClipComponent (const ClipInfo& clip)
{
    auto component = std::make_unique<ClipComponent> (context, clip.id);
    component->showClip (clip);

    if (clip.kind == ClipType::audio)
    {
        auto& waveform = waveforms[clip.id];

        if (waveform == nullptr)
            waveform = makeClipWaveform (context, clip.id, *lanes); // which it repaints as it loads

        component->showWaveform (waveform.get());
    }

    showLookOf (*component);
    return component;
}

int Playlist::getZOrderFor (const juce::Component& layer, size_t index) const
{
    // Before the first of the layer's clips that comes after it in the list, so that clips that overlap show as the tools
    // find them.
    for (int i = 0; i < layer.getNumChildComponents(); ++i)
        if (const auto* other = dynamic_cast<const ClipComponent*> (layer.getChildComponent (i)))
            if (const auto found = clipIndex.find (other->getClip()); found != clipIndex.end() && found->second > index)
                return i;

    return -1;
}

void Playlist::updateClips()
{
    for (const auto& [id, component] : clipComponents)
        showLookOf (*component);
}

void Playlist::showLookOf (ClipComponent& component) const
{
    // A clip shows in its track's colour, as the track's header does, frosted if the track is frozen.
    const auto row = getRowOf (component.getInfo().track);
    component.showLook (row >= 0 ? rows[(size_t) row].track.colour : juce::Colours::grey, selection.isSelected (component.getClip()),
                        row >= 0 && rows[(size_t) row].track.frozen);
}

juce::Rectangle<int> Playlist::getClipBounds (const ClipInfo& clip) const
{
    const auto row = getRowOf (clip.track);

    if (row < 0)
        return {};

    const auto& r = rows[(size_t) row];
    const auto& timeline = getView();
    const auto left = juce::roundToInt (timeline.beatToX (clip.range.getStart()));
    const auto right = juce::roundToInt (timeline.beatToX (clip.range.getEnd()));
    return { left, getRowTop (r) + 1, std::max (2, right - left), std::max (1, r.height - 3) };
}

const ClipInfo* Playlist::getClipAt (juce::Point<int> position) const
{
    // In the pinned band, only the pinned tracks' clips. A clip shows over those before it in the list, so the last one
    // there is the one that is clicked.
    const auto inBand = position.y < pinnedHeight;

    for (auto i = shownClips.size(); i-- > 0;)
    {
        const auto& clip = shownClips[i];
        const auto row = getRowOf (clip.track);

        if (row >= 0 && rows[(size_t) row].track.pinned == inBand && getClipBounds (clip).contains (position))
            return &clip;
    }

    return nullptr;
}
} // namespace phazite::ui
