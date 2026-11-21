// SPDX-License-Identifier: AGPL-3.0-or-later
// A spike: a clip's notes kept as immutable values (the immer library, Boost Software License 1.0), measured beside the
// song's own notes, for the question whether the song should become an immutable value (docs/design/decisions.md,
// "Engineering"; the answer: roadmap step S5.20). A benchmark, which ctest skips:
//   phazite_tests.exe --test-suite=benchmark --no-skip --test-case="*immutable values*"
// What it leaves out, on purpose: what tells the views and the plan of a change (today the tree's listeners, which these
// times include for the song's index; for immutable values, a comparison of the roots, not written), a clip's aliases, and
// what an edit of notes does besides writing them.
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"

#include <immer/algorithm.hpp>
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

#if JUCE_WINDOWS
 #include <malloc.h>
#endif

namespace
{
double millisecondsOf (const std::function<void()>& f)
{
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

// The bytes the program holds from its heap, counted block by block: what a step kept costs is the difference, whatever
// the heap had free before. Nothing where the heap can't be walked.
std::int64_t getHeapBytesInUse()
{
   #if JUCE_WINDOWS
    _HEAPINFO info;
    info._pentry = nullptr;
    std::int64_t used = 0;

    while (_heapwalk (&info) == _HEAPOK)
        if (info._useflag == _USEDENTRY)
            used += (std::int64_t) info._size;

    return used;
   #else
    return 0;
   #endif
}

// A note as an immutable song would hold it: the values a NOTE element holds (Schema.h), in their own types.
struct ValueNote
{
    std::uint64_t id = 0;
    int pitch = 60;
    double start = 0.0, length = 1.0; // among its clip's notes
    int velocity = 100;
    bool muted = false, slide = false, portamento = false;
    std::uint8_t channel = 1;
};

// A clip's notes as a value: in the clip's order, and where each is by its ID. A change of values leaves the places as
// they are, so the two keep sharing them.
struct ValueClip
{
    immer::flex_vector<ValueNote> notes;
    immer::map<std::uint64_t, std::uint32_t> places;
};

struct Timed
{
    explicit Timed (juce::String rowName)
        : name (std::move (rowName))
    {
    }

    juce::String name;
    std::vector<double> times;

    double getMedian()
    {
        std::sort (times.begin(), times.end());
        return times[times.size() / 2];
    }
};

juce::String makeTable (const juce::String& title, std::vector<Timed*> rows, int count)
{
    juce::String text;
    text << "\n| " << title << " | Median | Least | For each note |\n|---|---|---|---|\n";

    for (auto* row : rows)
    {
        const auto median = row->getMedian();
        text << "| " << row->name << " | " << juce::String (median, 4) << " ms | " << juce::String (row->times.front(), 4) << " ms | "
             << juce::String (median * 1.0e6 / count, 1) << " ns |\n";
    }

    return text;
}
} // namespace

TEST_CASE (
    "Benchmark: a clip's 5,000 notes as the song holds them and as immutable values: read, changed, undone, and what a step kept costs"
    * doctest::test_suite ("benchmark") * doctest::skip())
{
    using phazite::test::at;
    using phazite::test::lasting;

    constexpr int count = 5000;
    const auto startAt = [] (size_t i) { return (double) i * 0.25 + 0.03; };
    const auto pitchAt = [] (size_t i) { return 54 + (int) ((i * 7) % 12); };

    //==========================================================================
    // The same notes twice: in a song, and as a value.
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    const auto clip = phazite::insertMidiClip (*song, track, { at (0.0), at (count * 0.25) }, nullptr);
    std::vector<phazite::NoteData> made;

    for (size_t i = 0; i < (size_t) count; ++i)
    {
        phazite::NoteData note;
        note.clip = clip;
        note.pitch = pitchAt (i);
        note.start = at (startAt (i));
        note.length = lasting (0.2);
        made.push_back (note);
    }

    phazite::setClipNotes (*song, clip, made);
    auto& undo = song->getUndoManager();
    undo.clearUndoHistory();
    const auto refs = getRefs (phazite::getClipNotes (*song, clip));
    REQUIRE (refs.size() == (size_t) count);

    ValueClip value;

    {
        auto notes = value.notes.transient();
        auto places = value.places.transient();

        for (size_t i = 0; i < refs.size(); ++i)
        {
            ValueNote note;
            note.id = refs[i].note.getRaw();
            note.pitch = pitchAt (i);
            note.start = startAt (i);
            note.length = 0.2;
            notes.push_back (note);
            places.set (refs[i].note.getRaw(), (std::uint32_t) i);
        }

        value.notes = notes.persistent();
        value.places = places.persistent();
    }

    // The notes in another order than the clip holds them, as a selection's are: each found by its ID. Their places in the
    // clip beside them, for what each is given.
    std::vector<size_t> order ((size_t) count);

    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;

    juce::Random random (20261121);

    for (size_t i = order.size(); i > 1; --i)
        std::swap (order[i - 1], order[(size_t) random.nextInt ((int) i)]);

    juce::String report;

    //==========================================================================
    // What a step kept for Undo holds, measured first, while the heap is as the notes left it: so many steps made and kept,
    // each changing two values (start and pitch) of so many notes, and the heap's bytes in use before and after.
    {
        auto change = 0;

        const auto changeSong = [&] (size_t notes)
        {
            undo.beginNewTransaction();
            ++change;
            std::vector<phazite::NoteData> placed;

            for (size_t n = 0; n < notes; ++n)
            {
                const auto i = order[n];
                placed.push_back ({ .clip = clip,
                                    .id = refs[i].note,
                                    .pitch = pitchAt (i + (size_t) change),
                                    .start = at (startAt (i) + change * 0.001),
                                    .length = lasting (0.2) });
            }

            phazite::placeNotes (*song, placed);
        };

        std::vector<ValueClip> history;

        const auto changeValue = [&] (size_t notes)
        {
            history.push_back (value);
            ++change;
            auto edited = value.notes.transient();

            for (size_t n = 0; n < notes; ++n)
            {
                const auto i = order[n];
                edited.update (*value.places.find (refs[i].note.getRaw()),
                               [&] (ValueNote note)
                               {
                                   note.start = startAt (i) + change * 0.001;
                                   note.pitch = pitchAt (i + (size_t) change);
                                   return note;
                               });
            }

            value.notes = edited.persistent();
        };

        report << "\n| What a step kept for Undo holds, two values changed of | The song (its undo actions) | Immutable values (the root "
                  "before) |\n|---|---|---|\n";

        for (const auto& [notes, steps, what] :
             { std::tuple { (size_t) count, 20, "every note of the 5,000" },
               std::tuple { (size_t) 500, 40, "500 notes, anywhere in the clip" }, std::tuple { (size_t) 1, 400, "one note" } })
        {
            history.reserve (history.size() + (size_t) steps);
            const auto before = getHeapBytesInUse();

            for (int s = 0; s < steps; ++s)
                changeSong (notes);

            const auto between = getHeapBytesInUse();

            for (int s = 0; s < steps; ++s)
                changeValue (notes);

            const auto after = getHeapBytesInUse();
            const auto kilobytes = [stepCount = steps] (std::int64_t bytes)
            { return juce::String ((double) bytes / stepCount / 1024.0, 2) + " KB"; };
            report << "| " << what << " | " << kilobytes (between - before) << " | " << kilobytes (after - between) << " |\n";
        }

        // Back to the notes as they were made, for the times below.
        while (undo.undo())
        {
        }

        undo.clearUndoHistory();
        value = history.front();
        history.clear();
    }

    //==========================================================================
    // Times: each way in turns, 41 passes, so that what the machine does meanwhile falls on all of them alike.
    constexpr int passes = 41;

    Timed songWalk { "The song: the clip's notes read (getClipNotes())" };
    Timed valueWalk { "Immutable values: the notes walked (immer::for_each)" };
    Timed valueLoop { "Immutable values: the notes walked (a range-for)" };
    Timed songByID { "The song: each note found by its ID" };
    Timed valueByID { "Immutable values: each note found by its ID" };
    auto read = 0.0, wanted = 0.0;

    for (int pass = 0; pass < passes; ++pass)
    {
        songWalk.times.push_back (millisecondsOf (
            [&]
            {
                auto sum = 0.0;
                for (const auto& note : phazite::getClipNotes (*song, clip))
                    sum += note.start.inBeats() + note.length.inBeats() + note.pitch;

                wanted = sum;
            }));

        valueWalk.times.push_back (millisecondsOf (
            [&]
            {
                auto sum = 0.0;
                immer::for_each (value.notes, [&sum] (const ValueNote& note) { sum += note.start + note.length + note.pitch; });
                read = sum;
            }));
        CHECK (read == doctest::Approx (wanted));

        valueLoop.times.push_back (millisecondsOf (
            [&]
            {
                auto sum = 0.0;

                for (const auto& note : value.notes)
                    sum += note.start + note.length + note.pitch;

                read = sum;
            }));
        CHECK (read == doctest::Approx (wanted));

        songByID.times.push_back (millisecondsOf (
            [&]
            {
                auto sum = 0.0;

                for (const auto i : order)
                {
                    const auto note = phazite::getNoteData (*song, refs[i]);
                    sum += note->start.inBeats() + note->length.inBeats() + note->pitch;
                }

                read = sum;
            }));
        CHECK (read == doctest::Approx (wanted));

        valueByID.times.push_back (millisecondsOf (
            [&]
            {
                auto sum = 0.0;

                for (const auto i : order)
                {
                    const auto& note = value.notes[*value.places.find (refs[i].note.getRaw())];
                    sum += note.start + note.length + note.pitch;
                }

                read = sum;
            }));
        CHECK (read == doctest::Approx (wanted));
    }

    report << makeTable ("Three values of each of 5,000 notes read", { &songWalk, &valueWalk, &valueLoop, &songByID, &valueByID }, count);

    //==========================================================================
    // Two values of every note changed (start and pitch), each found by its ID, as one undo step; then that step undone.
    Timed songEdit { "The song: one edit of them all (placeNotes()), in the undo history" };
    Timed valueEditEach { "Immutable values: a new root for each note changed, the root before kept" };
    Timed valueEditBatch { "Immutable values: every note changed in one batch (a transient), the root before kept" };
    Timed songUndo { "The song: that step undone" };
    Timed valueUndo { "Immutable values: that step undone (the root before put back)" };
    auto change = 0;

    const auto editSong = [&]
    {
        undo.beginNewTransaction();
        ++change;
        std::vector<phazite::NoteData> placed;

        for (const auto i : order)
            placed.push_back ({ .clip = clip,
                                .id = refs[i].note,
                                .pitch = pitchAt (i + (size_t) change),
                                .start = at (startAt (i) + change * 0.001),
                                .length = lasting (0.2) });

        phazite::placeNotes (*song, placed);
    };

    const auto changed = [&] (size_t i)
    {
        return [&, i] (ValueNote note)
        {
            note.start = startAt (i) + change * 0.001;
            note.pitch = pitchAt (i + (size_t) change);
            return note;
        };
    };

    for (int pass = 0; pass < passes; ++pass)
    {
        songEdit.times.push_back (millisecondsOf ([&] { editSong(); }));
        songUndo.times.push_back (millisecondsOf ([&] { REQUIRE (undo.undo()); }));
        undo.clearUndoHistory(); // not timed: the step's memory let go

        for (const auto batch : { false, true })
        {
            ValueClip before, undone;

            (batch ? valueEditBatch : valueEditEach)
                .times.push_back (millisecondsOf (
                    [&]
                    {
                        before = value; // what Undo puts back
                        ++change;

                        if (batch)
                        {
                            auto edited = value.notes.transient();

                            for (const auto i : order)
                                edited.update (*value.places.find (refs[i].note.getRaw()), changed (i));

                            value.notes = edited.persistent();
                        }
                        else
                        {
                            for (const auto i : order)
                                value.notes = value.notes.update (*value.places.find (refs[i].note.getRaw()), changed (i));
                        }
                    }));

            const auto putBack = millisecondsOf (
                [&]
                {
                    undone = std::move (value); // kept, as Redo would keep it
                    value = before;
                });

            if (batch)
                valueUndo.times.push_back (putBack);
        }
    }

    // Both ended where they began.
    auto sumOfSong = 0.0, sumOfValue = 0.0;
    for (const auto& note : phazite::getClipNotes (*song, clip))
        sumOfSong += note.start.inBeats() + note.pitch;
    immer::for_each (value.notes, [&] (const ValueNote& note) { sumOfValue += note.start + note.pitch; });
    CHECK (sumOfSong == doctest::Approx (sumOfValue));

    report << makeTable ("Two values of each of 5,000 notes changed, as one undo step", { &songEdit, &valueEditEach, &valueEditBatch },
                         count);
    report << makeTable ("That step undone", { &songUndo, &valueUndo }, count);
    MESSAGE (report);
}
