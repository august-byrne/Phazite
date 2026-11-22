// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's clips change hears it from one place, by IDs (model/base/internal/SongValues.h: ClipsListener),
// which the song's keeper tells from its tree's notifications while the tree holds the clips (roadmap step S5.20). For each
// kind of change made through the model's functions, the calls it tells, folded per undo step into what the clips' area will
// tell once it holds them (one call for each step: each clip's track before the step and after it, and the tracks whose
// clips came in another order), are what the clips' values say changed; its Undo tells the mirror of it, its Redo the same.
//   phazite_tests.exe --test-case="Song values' clips*"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clipboard.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
using phazite::ClipID;
using phazite::TrackID;
using phazite::test::at;

// Every call the clips' listener heard, in order.
struct Told final : phazite::SongValues::ClipsListener
{
    struct Call
    {
        std::vector<phazite::SongValues::ClipChange> clips;
        std::vector<TrackID> reordered;
    };

    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>& clips, const std::vector<TrackID>& reordered) override
    {
        calls.push_back ({ clips, reordered });
    }

    std::vector<Call> calls;
};

// What a step changed of the clips, as one change of the clips' area tells it: each clip by its track before and after (a
// clip added and taken out within it is none), and the tracks whose clips came in another order.
struct Folded
{
    std::map<ClipID, std::pair<TrackID, TrackID>> clips;
    std::set<TrackID> reordered;

    bool isEmpty() const { return clips.empty() && reordered.empty(); }

    std::string describe() const
    {
        juce::StringArray parts;

        for (const auto& [clip, tracks] : clips)
            parts.add (juce::String ((juce::int64) clip.getRaw()) + ": " + juce::String ((juce::int64) tracks.first.getRaw()) + " to "
                       + juce::String ((juce::int64) tracks.second.getRaw()));

        for (const auto track : reordered)
            parts.add ("reordered " + juce::String ((juce::int64) track.getRaw()));

        return parts.joinIntoString ("; ").toStdString();
    }

    // As its undo tells it: each clip from where it is after to where it was before.
    Folded mirrored() const
    {
        auto mirror = *this;

        for (auto& entry : mirror.clips)
            std::swap (entry.second.first, entry.second.second);

        return mirror;
    }
};

// The calls folded: each clip's first track before, and its last after; the reordered tracks once each. What the clips' area
// tells in one call once it holds the clips, so that these cases pass unchanged then.
Folded fold (const std::vector<Told::Call>& calls)
{
    Folded folded;

    for (const auto& call : calls)
    {
        for (const auto& change : call.clips)
            if (const auto [entry, added] = folded.clips.try_emplace (change.id, change.before, change.after); ! added)
                entry->second.second = change.after;

        folded.reordered.insert (call.reordered.begin(), call.reordered.end());
    }

    std::erase_if (folded.clips, [] (const auto& entry) { return ! entry.second.first.isValid() && ! entry.second.second.isValid(); });
    return folded;
}

// The song's clips as values, each by its ID, and each track's in its order.
struct Clips
{
    std::map<ClipID, phazite::ClipValue> values;
    std::map<TrackID, std::vector<ClipID>> tracks;

    explicit Clips (const phazite::Song& song)
    {
        for (const auto& clip : phazite::getClipValues (song))
        {
            values.emplace (clip.id, clip);
            tracks[clip.track].push_back (clip.id);
        }
    }
};

// What the values say changed from one to the other: each clip added, taken out, moved or changed, and each track that holds
// the same clips in another order.
Folded findChanged (const Clips& before, const Clips& after)
{
    Folded changed;
    std::set<ClipID> ids;

    for (const auto& entry : before.values)
        ids.insert (entry.first);

    for (const auto& entry : after.values)
        ids.insert (entry.first);

    for (const auto id : ids)
    {
        const auto was = before.values.find (id);
        const auto is = after.values.find (id);
        const auto from = was != before.values.end() ? was->second.track : TrackID();
        const auto to = is != after.values.end() ? is->second.track : TrackID();

        if (was == before.values.end() || is == after.values.end() || ! (was->second == is->second))
            changed.clips[id] = { from, to };
    }

    for (const auto& [track, list] : after.tracks)
    {
        if (const auto was = before.tracks.find (track); was != before.tracks.end() && was->second != list)
        {
            auto a = was->second, b = list;
            std::sort (a.begin(), a.end());
            std::sort (b.begin(), b.end());

            if (a == b)
                changed.reordered.insert (track);
        }
    }

    return changed;
}

// A song in a project of its own, its clips' listener recording what it tells.
struct Watched
{
    Watched() { song.getValues()->addClipsListener (&told); }
    ~Watched() { song.getValues()->removeClipsListener (&told); }

    // A change as one undo step (the change's own, joined), what it told folded and compared with what the values say it
    // changed (something), then its Undo (the mirror) and its Redo (the same). Returns the step's calls.
    std::vector<Told::Call> check (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());
        const Clips before (song);
        told.calls.clear();

        {
            const phazite::UndoStep step (song, name);
            change();
        }

        const Clips after (song);
        auto calls = std::exchange (told.calls, {});
        const auto step = fold (calls);
        CHECK_FALSE (step.isEmpty());
        CHECK (step.describe() == findChanged (before, after).describe());

        phazite::undo (song);
        CHECK (fold (std::exchange (told.calls, {})).describe() == step.mirrored().describe());
        CHECK (findChanged (Clips (song), before).isEmpty());

        phazite::redo (song);
        CHECK (fold (std::exchange (told.calls, {})).describe() == step.describe());
        CHECK (findChanged (Clips (song), after).isEmpty());
        return calls;
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told;
};

// A MIDI file of one middle C, a beat long.
juce::File writeMidi (const juce::File& file)
{
    juce::MidiMessageSequence sequence;
    sequence.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0.0);
    sequence.addEvent (juce::MidiMessage::noteOff (1, 60), 960.0);

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (960);
    midi.addTrack (sequence);

    REQUIRE (file.getParentDirectory().createDirectory());
    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (midi.writeTo (stream));
    return file;
}
} // namespace

TEST_CASE ("Song values' clips: each change through the model tells its clips and their tracks, its Undo the mirror, its Redo "
           "the same")
{
    Watched w;
    auto& song = w.song;
    const auto first = w.s.firstTrack;
    const auto second = w.s.addTrack ("Second");
    const auto clip = w.s.addMidiClip (first, 0.0, 4.0);
    w.s.addNote (clip, 60, 1.0, 1.0);
    const auto other = w.s.addMidiClip (first, 8.0, 4.0);

    SUBCASE ("A clip added")
    {
        ClipID added;
        w.check ("Add Clip", [&] { added = phazite::insertMidiClip (song, second, { at (0.0), at (4.0) }, nullptr); });
        CHECK (added.isValid());
    }

    SUBCASE ("A property set")
    {
        w.check ("Mute Clip", [&] { phazite::toggleClipsMuted (song, { clip }); });
    }

    SUBCASE ("A clip moved along its track, and to another")
    {
        w.check ("Move Clip", [&] { phazite::moveClip (song, clip, at (16.0), first); });
        const auto calls = w.check ("Move Clip", [&] { phazite::moveClip (song, clip, at (20.0), second); });
        CHECK (fold (calls).clips.at (clip) == std::pair { first, second });
    }

    SUBCASE ("Split")
    {
        w.check ("Split", [&] { CHECK (phazite::splitClips (song, { clip }, at (2.0)).size() == 1); });
    }

    SUBCASE ("Deleted")
    {
        w.check ("Delete Clip", [&] { phazite::deleteClips (song, { clip }, false); });
    }

    SUBCASE ("Made an alias, and made unique")
    {
        std::vector<ClipID> aliases;
        w.check ("Make Alias", [&] { aliases = phazite::copyClips (song, { clip }, true); });
        REQUIRE (aliases.size() == 1);
        w.check ("Make Unique", [&] { phazite::makeClipsUnique (song, { clip, aliases[0] }); });
    }

    SUBCASE ("A track with clips deleted, all its clips told in one call; and duplicated")
    {
        w.check ("Duplicate Track", [&] { CHECK (phazite::duplicateTracks (song, { first }, nullptr).size() == 1); });
        const auto calls = w.check ("Delete Track", [&] { phazite::deleteTracks (song, { first }, nullptr); });
        CHECK (std::any_of (calls.begin(), calls.end(),
                            [&] (const Told::Call& call)
                            {
                                const auto holds = [&call] (ClipID id)
                                {
                                    return std::any_of (call.clips.begin(), call.clips.end(),
                                                        [id] (const phazite::SongValues::ClipChange& change) { return change.id == id; });
                                };

                                return holds (clip) && holds (other);
                            }));
    }

    SUBCASE ("A folder with tracks deleted")
    {
        const auto onSecond = w.s.addMidiClip (second, 0.0, 2.0);
        REQUIRE (onSecond.isValid());
        TrackID folder;

        {
            const phazite::UndoStep step (song, "Group");
            folder = phazite::groupIntoFolder (song, { first, second }, nullptr);
        }

        REQUIRE (folder.isValid());
        const auto calls = w.check ("Delete Folder", [&] { phazite::deleteTracks (song, { folder }, nullptr); });
        CHECK (fold (calls).clips.size() == 3);
    }

    SUBCASE ("A paste")
    {
        phazite::Clipboard clipboard;
        w.s.select (other);
        phazite::copySelection (clipboard, getSelection (w.s.context), getRangeSelection (w.s.context));
        REQUIRE (clipboard.kind == phazite::Clipboard::Kind::clips);
        w.check ("Paste", [&] { phazite::paste (clipboard, w.s.context); });
    }

    SUBCASE ("An import")
    {
        const auto file = writeMidi (w.s.getSoundsFolder().getChildFile ("Middle C.mid"));
        ClipID imported;
        w.check ("Import", [&] { imported = phazite::importFile (song, second, file, at (4.0)); });
        CHECK (imported.isValid());
    }
}

TEST_CASE ("Song values' clips: a warp marker added to an audio clip tells the clip changed in place")
{
    Watched w;
    const auto track = w.s.addTrack ("Audio");
    const auto sound = phazite::test::writeSound (w.s.getSoundsFolder().getChildFile ("Sine.wav"), 2.0);
    const auto clip = w.s.addAudioClip (track, sound, 0.0);
    REQUIRE (clip.isValid());

    const auto calls = w.check ("Add Warp Marker", [&] { CHECK (phazite::addWarpMarker (w.song, clip, 0.5)); });
    CHECK (fold (calls).clips.at (clip) == std::pair { track, track });
    CHECK (phazite::getWarpMarkers (w.song, clip).size() >= 1);
}

TEST_CASE ("Song values' clips: what listens to the song's tree hears nothing of a clip, and the clips' listener nothing else")
{
    // isHeardAsClips() says which of the tree's notifications are the clips' listener's.
    Watched w;
    const auto clip = w.s.addMidiClip (w.s.firstTrack, 0.0, 4.0);
    const auto clipState = phazite::findClipState (w.song, clip);
    const auto trackState = phazite::findTrackState (w.song, w.s.firstTrack);
    REQUIRE (clipState.isValid());

    CHECK (phazite::isHeardAsClips (clipState));
    CHECK (phazite::isHeardAsClips (trackState, clipState));
    CHECK_FALSE (phazite::isHeardAsClips (trackState));
    CHECK_FALSE (phazite::isHeardAsClips (w.song.getState()));

    // What a listener of the song's root hears that isn't the clips' (isHeardAsClips() says no), as the listeners that moved
    // onto the clips' listener leave the rest to their tree callbacks.
    struct TreeHeard final : juce::ValueTree::Listener
    {
        explicit TreeHeard (const juce::ValueTree& rootToHear)
            : root (rootToHear)
        {
            root.addListener (this);
        }
        ~TreeHeard() override { root.removeListener (this); }

        juce::ValueTree root;
        int heard = 0;

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override
        {
            if (! phazite::isHeardAsClips (tree))
                ++heard;
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
        {
            if (! phazite::isHeardAsClips (parent, child))
                ++heard;
        }

        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
        {
            if (! phazite::isHeardAsClips (parent, child))
                ++heard;
        }

        void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int newIndex) override
        {
            if (! phazite::isHeardAsClips (parent, parent.getChild (newIndex)))
                ++heard;
        }
    };

    TreeHeard tree (w.song.getState());

    // A clip's property set, and a clip renamed: through the tree, nothing; to the clips' listener, one call each, which
    // tells that clip alone, once (nothing told twice for one notification of the tree).
    const auto toldOnce = [&w, &tree, clip] (const std::function<void()>& edit)
    {
        tree.heard = 0;
        w.told.calls.clear();
        edit();
        CHECK (tree.heard == 0);
        REQUIRE (w.told.calls.size() == 1);
        REQUIRE (w.told.calls[0].clips.size() == 1);
        CHECK (w.told.calls[0].clips[0].id == clip);
        CHECK (w.told.calls[0].clips[0].before == w.s.firstTrack);
        CHECK (w.told.calls[0].clips[0].after == w.s.firstTrack);
        CHECK (w.told.calls[0].reordered.empty());
    };

    toldOnce ([&w, clip] { phazite::toggleClipsMuted (w.song, { clip }); });
    toldOnce ([&w, clip] { phazite::renameClips (w.song, { clip }, "Renamed"); });

    // A track's name: through the tree; nothing told the clips' listener.
    tree.heard = 0;
    w.told.calls.clear();
    phazite::setTrackName (w.song, w.s.firstTrack, "Renamed");
    CHECK (tree.heard > 0);
    CHECK (w.told.calls.empty());
}
