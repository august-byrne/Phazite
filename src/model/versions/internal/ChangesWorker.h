// SPDX-License-Identifier: AGPL-3.0-or-later
// The changes of the open song against a version's song (its base's, or one compared with it: VersionHistory.h), worked out
// on the history's thread (docs/design/versions.md: Off the message thread). The message thread marks what the song's
// notifications say changed (a track, the master, an automation curve, the song's own, its tracks' order and names), and
// hands over, a round at a time, copies of only those parts (take()); the history's thread compares them with the version's
// (work()), keeps what it found for each part, and gives back the whole list; the message thread keeps the last list
// (takeResult()), and works out none of it. A track that didn't change is never copied or compared again. One round under
// way at a time: what changes meanwhile goes into the next.
#pragma once

#include "SongDiff.h"
#include "model/base/Song.h"

#include <juce_data_structures/juce_data_structures.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace phazite
{
class ChangesWorker
{
public:
    // The key the master's line is marked, copied and compared by, whatever its ID (a version may give it another: known bug
    // 244, fixed): no track's ID is 0. Its line is named by its ID now (Request::master).
    static constexpr std::uint64_t masterKey = 0;

    // What the history's thread keeps between rounds, for one version's song (a reference): its song, never changed, and
    // what each part compared to; only that thread touches it once given.
    struct State;

    // A round: what changed since the last, copied on the message thread.
    struct Request
    {
        std::shared_ptr<State> state;
        int epoch = 0, number = 0;
        bool all = false;         // the whole song, copied (songNow): everything compared again
        juce::ValueTree songNow;  // all: the song; else, with skeleton, the song's own parts and its tracks' order (or none)
        bool skeleton = false;    // songNow holds the song's own and its tracks' names and order
        std::uint64_t master = 0; // the master's ID now, which its line is named by
        std::map<std::uint64_t, juce::ValueTree> tracks; // tracks and the master (masterKey) that changed, as they are (none: gone)
        std::map<juce::String, juce::ValueTree> curves;  // automation curves that changed, by key, as they are (none: gone)
        PluginParameterValues parameters;                // the plug-ins' knobs in what was copied, as they are now
        PluginParameterValues opened;                    // knobs of the reference's plug-ins that loaded after it was read
        std::set<std::uint64_t> reported;                // the plug-ins that said they changed since the reference
        bool withReported = true;                        // with it (the version the song grew from); without, every state counts
    };

    struct Result
    {
        int epoch = 0, number = 0;
        std::vector<SongChange> changes;
    };

    // Message thread. A reference to compare with: a new one (another version's song, its plug-ins' knobs), after which
    // everything is compared again unless the song is known to be the same as it (known: a version just saved of it).
    void setReference (const juce::ValueTree& song, PluginParameterValues parameters, bool withReported, bool knownTheSame = false);
    bool hasReference() const noexcept { return state != nullptr; }
    void clear(); // none: no changes, nothing to work out

    // What changed, as the song notified it: a track by its ID or the master by masterKey, an automation curve by its key
    // (schema::getElementKey()), the song's own (its properties, tempo map, sections, chords), its tracks' order or names,
    // and anything (every part copied again).
    void markTrack (std::uint64_t id);
    void markCurve (const juce::String& key);
    void markSong();
    void markOrder();
    void markAll();

    // Whether something changed that no round took yet, and whether a round is under way.
    bool isDirty() const noexcept { return all || order || songOwn || ! tracks.empty() || ! curves.empty(); }
    bool isWorking() const noexcept { return working; }

    // Whether the last list is the song's as it is: nothing changed since the round it came from, which is in.
    bool isUpToDate() const noexcept { return state == nullptr || (hasResult && ! working && ! isDirty()); }

    // A round's copies of what changed in the song (its plug-ins' knobs read through readKnobs, by the devices in what was
    // copied; the reference's own knobs for those that weren't known as it was read, through readOpened; and the plug-ins
    // that said they changed), which leaves nothing dirty, and marks a round under way. None while one is, or without a
    // reference.
    using KnobReader = std::function<PluginParameterValues (const std::set<std::uint64_t>& devices)>;
    std::shared_ptr<Request> take (const Song&, const KnobReader& readKnobs, const KnobReader& readOpened,
                                   std::set<std::uint64_t> reported);

    // The round's list, taken in: false for one of another reference (or none), which is dropped. The round is over either way.
    bool takeResult (const Result&);
    const std::vector<SongChange>& getChanges() const noexcept { return changes; }

    // The history's thread: a round worked out, against what the rounds before left.
    static Result work (const Request&);

    // How many tracks (and masters) the rounds compared since the program started, on any thread, and how many parts the
    // message thread copied for them: what the performance gates count (tests/PerformanceGateTests.cpp).
    static juce::int64 countTracksCompared();
    static juce::int64 countTracksCopied();

private:
    std::shared_ptr<State> state;
    int epoch = 0, number = 0;
    bool all = false, order = false, songOwn = false, working = false, hasResult = false;
    bool reportedToo = true; // setReference()'s withReported
    std::set<std::uint64_t> tracks;
    std::set<juce::String> curves;
    std::vector<SongChange> changes;
};
} // namespace phazite
