// SPDX-License-Identifier: AGPL-3.0-or-later
// Where playback loops (docs/design.md, section 6). As in FL Studio's song mode, the loop is the whole song
// unless something else is looped on purpose: from the song's start to the end of the bar its last clip ends
// in, so the loop grows as clips go later. A time range selected in the playlist, or a section, can be looped
// instead. Tracktion keeps the loop in the transport, without undo steps.
#pragma once

#include "Commands.h"
#include "Selection.h"

namespace phazite
{
// The song as a loop: from its start to the end of the bar its last clip ends in, and at least a bar long.
// Sections don't count: an outline can be longer than what is in it so far.
te::TimeRange getSongLoopRange (const te::Edit&);

// Whether the loop is the song's, and follows it, rather than a range looped on purpose. Kept in the project.
bool doesLoopFollowSong (const te::Edit&);

// Loops the song, or a range of it. Both turn looping on.
void loopSong (te::Edit&);
void loopRange (te::Edit&, te::TimeRange);

// Puts the loop around the song again if it follows the song and the song's end has moved.
void updateSongLoop (te::Edit&);

//==============================================================================
// Keeps an Edit's loop around its song while the loop follows it: when clips are added, moved, trimmed or
// deleted, and when the tempo changes. Each open project has one.
class SongLoopFollower : private juce::ValueTree::Listener,
                         private juce::AsyncUpdater
{
public:
    explicit SongLoopFollower (te::Edit&);
    ~SongLoopFollower() override;

private:
    te::Edit& edit;
    juce::ValueTree editState;

    void changed (const juce::ValueTree&);
    void handleAsyncUpdate() override { updateSongLoop (edit); }
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { changed (tree); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override { changed (child); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override { changed (child); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongLoopFollower)
};

//==============================================================================
// The loop's commands: Loop (on or off), Loop Selection (the time range selected in the playlist), Loop
// Section (the selected sections) and Loop the Song.
void addLoopCommands (CommandRegistry&, te::SelectionManager&, RangeSelection&);
} // namespace phazite
