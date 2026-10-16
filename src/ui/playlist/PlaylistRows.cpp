// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's tracks shown as rows, and their clips on them.
#include "PlaylistParts.h"
#include "TrackHeights.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Tracks.h"

namespace phazite::ui
{
namespace
{
    constexpr int focusedOutHeight = 20; // in focus mode, the tracks out of focus are this tall
} // namespace

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
            // A clip shows in its track's colour, as the track's header does.
            const auto row = getRowOf (clip->getTrack());
            component->update (row >= 0 ? rows[(size_t) row].colour : juce::Colours::grey, selection.isSelected (clip));
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
            component->update (row >= 0 ? rows[(size_t) row].colour : juce::Colours::grey, selection.isSelected (clip));
        }
    }
}
} // namespace phazite::ui
