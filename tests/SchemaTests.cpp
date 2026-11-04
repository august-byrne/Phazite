// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's state in a project file (src/model/base/internal/Schema.h).
#include "TestHelpers.h"
#include "model/base/internal/Schema.h"

TEST_CASE ("A song numbers its items from its own count, which goes past every ID it holds")
{
    phazite::Song song { juce::ValueTree (phazite::schema::PHAZITE) };
    CHECK (song.makeID() == 1);
    CHECK (song.makeID() == 2);
    CHECK ((juce::int64) song.getState()[phazite::schema::nextID] == 3);

    // An item with a larger ID than the count (as a song read from a file can hold): counting the IDs moves the count past
    // it, and the next ID comes after it.
    juce::ValueTree tracks (phazite::schema::TRACKS);
    juce::ValueTree track (phazite::schema::TRACK);
    track.setProperty (phazite::schema::trackID, (juce::int64) 10, nullptr);
    track.setProperty (phazite::schema::channelFader, (juce::int64) 11, nullptr);
    tracks.appendChild (track, nullptr);
    song.getState().appendChild (tracks, nullptr);
    CHECK (phazite::findNextID (song.getState()) == 12);
    phazite::countIDs (song);
    CHECK ((juce::int64) song.getState()[phazite::schema::nextID] == 12);
    CHECK (song.makeID() == 12);

    // A count further on than every ID stays where it is.
    song.getState().setProperty (phazite::schema::nextID, (juce::int64) 50, nullptr);
    phazite::countIDs (song);
    CHECK (song.makeID() == 50);
}
