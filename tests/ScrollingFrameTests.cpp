// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist and the piano roll frame by frame while they follow the playhead, painted as the screen shows them (through
// Direct2D on Windows): nothing beside the time line changes, what the time line shows moves exactly as far as the song
// played, and what stays in view stays put on the screen's pixels. Each of these broke as the owner watched the song play
// (docs/performance-audit.md, section 16).
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/chords/ChordModel.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/LookAndFeel.h"

#include <array>
#include <optional>

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// Parts of a pixel the song is past a whole one at a frame, either side of a half: not multiples of 1/64, which round
// exactly where the renderer's own rounding doesn't (docs/performance-audit.md, section 16, "The rain that blinked").
constexpr std::array<double, 6> parts { 0.137, 0.462, 0.731, 0.918, 0.283, 0.594 };

// Where two frames differ in an area, by more than the rounding of a channel: how many pixels, and the first. Each pixel of
// the second is compared with the one so many columns right of it in the first (as far as what it shows moved left), but in
// the columns skipped, in either frame (the playhead's, which stays on its column).
struct Difference
{
    int pixels = 0;
    juce::Point<int> first { -1, -1 };
};

Difference compare (const juce::Image& before, const juce::Image& after, juce::Rectangle<int> area, int moved = 0,
                    juce::Range<int> skipped = {})
{
    const auto near = [] (juce::uint8 x, juce::uint8 y) { return std::abs ((int) x - (int) y) <= 2; };
    const juce::Image::BitmapData pixelsBefore (before, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData pixelsAfter (after, juce::Image::BitmapData::readOnly);
    Difference difference;

    for (auto y = area.getY(); y < area.getBottom(); ++y)
        for (auto x = area.getX(); x + moved < area.getRight(); ++x)
        {
            if (skipped.contains (x) || skipped.contains (x + moved))
                continue;

            const auto p = pixelsBefore.getPixelColour (x + moved, y), q = pixelsAfter.getPixelColour (x, y);

            if (near (p.getRed(), q.getRed()) && near (p.getGreen(), q.getGreen()) && near (p.getBlue(), q.getBlue())
                && near (p.getAlpha(), q.getAlpha()))
                continue;

            if (difference.pixels++ == 0)
                difference.first = { x, y };
        }

    return difference;
}

// A song of two tracks playing, open in a context as the app has it, which the editors show.
struct PlayingSong
{
    PlayingSong()
    {
        tracks = phazite::ensureTracks (*song, 2);
        clip = phazite::insertMidiClip (*song, tracks[0], { beats (0.0), beats (256.0) }, nullptr);
        REQUIRE (clip.isValid());
        song->getUndoManager().clearUndoHistory();
        setSong (context, song.get());
        phazite::test::dispatchPendingMessages();
    }

    ~PlayingSong()
    {
        if (phazite::isSongPlaying (*song))
            phazite::playOrStop (*song);

        setSong (context, nullptr);
    }

    // Something of everything the editors draw along the time line, between beats 33 and 47, which stay in view while the song
    // plays on from beat 40 with 16 beats in view: the first track's clip's notes (which the piano roll shows), a clip of notes
    // on the second (the piano roll's ghosts), the second track's volume automated (ramps, a bend) and the first's pan shown
    // without points (its dashed line), a section, chords and tempo changes.
    void fill()
    {
        auto& s = *song;

        for (auto beat = 33.0; beat < 47.0; beat += 1.25)
            phazite::addNote (s, clip, 52 + (int) (beat * 4.0) % 24, beats (beat), phazite::BeatDuration::fromBeats (0.5),
                              40 + (int) (beat * 3.0) % 80);

        const auto other = phazite::insertMidiClip (s, tracks[1], { beats (41.25), beats (43.5) }, nullptr);
        REQUIRE (other.isValid());
        phazite::addNote (s, other, 64, beats (41.5), phazite::BeatDuration::fromBeats (1.0), 90);

        const auto volume = phazite::getVolume (context, tracks[1]);
        phazite::showAutomationLane (volume);

        for (const auto& [beat, value] :
             { std::pair { 33.5, 0.2f }, std::pair { 36.25, 0.8f }, std::pair { 38.0, 0.4f }, std::pair { 44.5, 0.6f } })
            phazite::addAutomationPoint (context, volume.getRef(), { beats (beat), value });

        phazite::showAutomationLane (phazite::getPan (context, tracks[0]));
        REQUIRE (phazite::insertSection (s, beats (34.0), { .name = "Verse", .length = phazite::BeatDuration::fromBeats (4.0) }).isValid());
        phazite::addLaneChord (context, { beats (36.0), beats (40.0) }, { 1, 0, phazite::ChordType::major, 0 });
        phazite::addLaneChord (context, { beats (40.0), beats (44.0) }, { 5, 0, phazite::ChordType::major, 0 });
        phazite::addTempoChange (s, beats (35.0), 128.0);
        phazite::addTempoChange (s, beats (42.5), 136.0);
        phazite::test::dispatchPendingMessages();
    }

    // Plays, the playhead held at a place: the tests' engine plays no blocks, so the song waits wherever it is put.
    void play()
    {
        phazite::playOrStop (*song);
        REQUIRE (phazite::isSongPlaying (*song));
        session = phazite::findPlayingSession (*song);
        REQUIRE (session != nullptr);
    }

    // A frame of the display with the song so many of the editor's pixels from its start.
    void showFrameAt (phazite::ui::TimelineEditor& editor, double pixels)
    {
        session->locate (beats (pixels / editor.getView().pixelsPerBeat));
        editor.showFrame (juce::Time::getMillisecondCounterHiRes() / 1000.0);
    }

    phazite::Song::Ptr song = phazite::createSong(); // 120 BPM, 4/4
    std::vector<phazite::TrackID> tracks;
    phazite::ClipID clip; // on the first track, from the start, longer than any view here
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::PlaybackSession* session = nullptr;
};

// An editor following the playhead continuously, 16 beats in view, the song past the middle of the view.
void followContinuously (phazite::ui::TimelineEditor& editor)
{
    editor.zoomTo ({ phazite::BeatPosition(), beats (16.0) });
    editor.setFollowingPlayback (true);
    editor.setScrollingContinuously (true);
}

// An editor scrolling continuously as the song plays on past the middle of its view, a part of a pixel past a whole one at
// each frame: everything left of its time line (track headers, piano keys, the corner over them) is the same in every frame.
void checkBesideTheTimelineStaysStill (PlayingSong& song, phazite::ui::TimelineEditor& editor, phazite::test::TestWindow& window,
                                       juce::Component& timelinePart)
{
    followContinuously (editor);
    const auto timelineLeft = editor.getLocalPoint (&timelinePart, juce::Point<int>()).x;
    REQUIRE (timelineLeft > 0);
    const juce::Rectangle<int> beside (0, 0, timelineLeft, editor.getHeight());

    song.play();
    const auto pixel = std::floor (40.0 * editor.getView().pixelsPerBeat);
    std::optional<juce::Image> first;

    for (const auto part : parts)
    {
        CAPTURE (part);
        song.showFrameAt (editor, pixel + part);
        REQUIRE (editor.getView().start.inBeats() > 0.0); // the song scrolls under the playhead
        REQUIRE_FALSE (juce::exactlyEqual (editor.getSubPixel(), 0.0f));
        const auto frame = window.paintEverything();

        if (! first.has_value())
        {
            first = frame;
            continue;
        }

        const auto difference = compare (*first, frame, beside);
        CAPTURE (difference.first.toString().toStdString());
        CHECK (difference.pixels == 0);
    }
}

// An editor scrolling continuously as the song plays on by whole pixels, each frame at the same part of a pixel past one:
// what each shows of the time line, in every part of it (the ruler, the lanes and what is on them), is the last one's moved
// left by as many pixels as the song went, but for the playhead's columns, which stay where they are. Anything placed where
// it is in view rather than where it is in the song (rounded there, or sampled at the view's pixels) moves otherwise.
void checkTimelineMovesWithTheSong (PlayingSong& song, phazite::ui::TimelineEditor& editor, phazite::test::TestWindow& window,
                                    juce::Component& timelinePart)
{
    followContinuously (editor);
    auto* overview = phazite::test::findComponentWithTitle (editor, "Overview");
    auto* playhead = phazite::test::findComponentWithTitle (editor, "Playhead");
    REQUIRE (overview != nullptr);
    REQUIRE (playhead != nullptr);

    // Under the overview (its thumb shows where the view is), from the time line's left edge to its right, short of a scroll bar
    // that shows over it (which stays where it is).
    const auto left = editor.getLocalPoint (&timelinePart, juce::Point<int>()).x;
    auto right = editor.getWidth();

    for (auto* bar : phazite::test::findAll<juce::ScrollBar> (editor))
        if (bar->isVisible())
            right = std::min (right, editor.getLocalArea (bar, bar->getLocalBounds()).getX());

    const auto top = editor.getLocalArea (overview, overview->getLocalBounds()).getBottom();
    const juce::Rectangle<int> timeline (left, top, right - left, editor.getHeight() - top);
    REQUIRE (timeline.getWidth() > 400);

    song.play();
    const auto first = std::floor (40.0 * editor.getView().pixelsPerBeat);

    for (const auto part : { 0.137, 0.731 })
    {
        std::optional<juce::Image> before;
        auto at = first;

        for (const auto step : { 0, 1, 6, 23 })
        {
            CAPTURE (part);
            CAPTURE (step);
            at += step;
            song.showFrameAt (editor, at + part);
            REQUIRE (editor.getSubPixel() == doctest::Approx (part).epsilon (1.0e-4));
            const auto frame = window.paintEverything();

            if (before.has_value())
            {
                const auto line = editor.getLocalArea (playhead, playhead->getLocalBounds());
                const auto difference = compare (*before, frame, timeline, step, { line.getX(), line.getRight() });
                CAPTURE (difference.first.toString().toStdString());
                CHECK (difference.pixels == 0);
            }

            before = frame;
        }
    }
}

// A playlist on a song, in a window.
struct PlaylistOn
{
    explicit PlaylistOn (PlayingSong& song)
        : playlist (commands, song.context)
    {
        phazite::test::dispatchPendingMessages();
        lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
        REQUIRE (lanes != nullptr);
    }

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Playlist playlist;
    phazite::test::TestWindow window { playlist, 1000, 600 };
    juce::Component* lanes = nullptr;
};

// A piano roll on a song, showing its first track's clip, in a window.
struct PianoRollOn
{
    explicit PianoRollOn (PlayingSong& song)
        : pianoRoll (commands, song.context)
    {
        getSelection (song.context).selectOnly (song.clip);
        phazite::test::dispatchPendingMessages();
        notes = phazite::test::findComponentWithTitle (pianoRoll, "Notes");
        REQUIRE (notes != nullptr);
    }

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::PianoRoll pianoRoll;
    phazite::test::TestWindow window { pianoRoll, 1000, 600 };
    juce::Component* notes = nullptr;
};
} // namespace

TEST_CASE ("Scrolling continuously, the track headers and the corner over them stay still while the song glides under the playhead")
{
    // The owner, 2026-11-11: the track headers' right edge flickered while the playlist followed the playhead. The lanes and
    // the time bar's parts, moved left by a part of a pixel, painted over the column beside them at every frame that moved them
    // that way, and not at the others.
    PlayingSong song;
    PlaylistOn shown (song);
    checkBesideTheTimelineStaysStill (song, shown.playlist, shown.window, *shown.lanes);
}

TEST_CASE ("Scrolling continuously, the piano keys and the velocity lane's name stay still while the song glides under the playhead")
{
    // As the playlist's headers: the notes and the velocities, moved left by a part of a pixel, painted over the column beside
    // them.
    PlayingSong song;
    PianoRollOn shown (song);
    checkBesideTheTimelineStaysStill (song, shown.pianoRoll, shown.window, *shown.notes);
}

TEST_CASE ("Scrolling continuously, everything on the playlist's time line moves with the song, as far as it played")
{
    // The owner, 2026-11-09 and 2026-11-10: every other grid line wiggled and clips' edges flickered, rounded where they were in
    // view; an automation lane's line wobbled, worked out at the view's pixels; and a lane's dashed line stood still, its dashes
    // started where each paint began, while the grid glided past it (2026-11-11).
    PlayingSong song;
    song.fill();
    PlaylistOn shown (song);
    checkTimelineMovesWithTheSong (song, shown.playlist, shown.window, *shown.lanes);
}

TEST_CASE ("Scrolling continuously, everything on the piano roll's time line moves with the song, as far as it played")
{
    PlayingSong song;
    song.fill();
    PianoRollOn shown (song);
    checkTimelineMovesWithTheSong (song, shown.pianoRoll, shown.window, *shown.notes);
}

TEST_CASE ("Scrolling continuously, the tags of clips the view's edges cut stay put on the screen while the clips glide under them")
{
    // A clip's name, and an alias's mark, stay in view while the view's edge cuts the clip, as FL Studio keeps a clip's label
    // on screen (ClipComponent::paintTags()): kept at the lanes' own edges, which scrolling continuously moves by parts of a
    // pixel, they went back and forth by up to a pixel at every frame.
    PlayingSong song;
    auto& s = *song.song;

    // With the playhead at beat 40 and 16 beats in view, from beat 32 to 48: a named clip across the left edge, and an alias
    // across the right.
    const auto named = phazite::insertMidiClip (s, song.tracks[1], { beats (24.0), beats (39.0) }, nullptr);
    const auto original = phazite::insertMidiClip (s, song.tracks[1], { beats (41.0), beats (43.0) }, nullptr);
    REQUIRE (named.isValid());
    REQUIRE (original.isValid());
    phazite::renameClips (s, { named }, "Hook");
    const auto aliases = phazite::copyClips (s, { original }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (s, aliases[0], beats (46.5), song.tracks[1]);
    phazite::test::dispatchPendingMessages();

    PlaylistOn shown (song);
    followContinuously (shown.playlist);
    const auto lanesArea = shown.playlist.getLocalArea (shown.lanes, shown.lanes->getLocalBounds()).withTrimmedRight (1);
    song.play();

    const auto pixel = std::floor (40.0 * shown.playlist.getView().pixelsPerBeat);
    std::optional<juce::Image> first;
    juce::Rectangle<int> name, mark;

    for (const auto part : parts)
    {
        CAPTURE (part);
        song.showFrameAt (shown.playlist, pixel + part);
        REQUIRE_FALSE (juce::exactlyEqual (shown.playlist.getSubPixel(), 0.0f));

        // Inside each tag, clear of its rounded corners: the name's at the left of what shows of its clip, the mark's at the
        // right of what shows of the alias, both on the clips' row.
        if (! first.has_value())
        {
            const phazite::ui::ClipComponent* clip = nullptr;

            for (auto* each : phazite::test::findAll<phazite::ui::ClipComponent> (shown.playlist))
                if (each->getClip() == named)
                    clip = each;

            REQUIRE (clip != nullptr);
            using phazite::ui::ClipLabelTint::height, phazite::ui::ClipLabelTint::margin;
            const auto row = shown.playlist.getLocalArea (clip, clip->getLocalBounds());
            const auto inside = (int) std::ceil (phazite::ui::ClipLabelTint::corner) + 2; // clear of a tag's rounded corners
            const auto marksWidth = phazite::ui::ClipLabelTint::iconSize + 2 * phazite::ui::ClipLabelTint::iconGap;
            const auto tagTop = row.getY() + margin + inside, tagHeight = height - 2 * inside;
            name = { lanesArea.getX() + margin + inside, tagTop, 12, tagHeight };
            mark = { lanesArea.getRight() - margin - marksWidth + inside, tagTop, marksWidth - 2 * inside, tagHeight };
        }

        const auto frame = shown.window.paintEverything();

        if (! first.has_value())
        {
            first = frame;
            continue;
        }

        const auto nameDifference = compare (*first, frame, name);
        const auto markDifference = compare (*first, frame, mark);
        CAPTURE (nameDifference.first.toString().toStdString());
        CAPTURE (markDifference.first.toString().toStdString());
        CHECK (nameDifference.pixels == 0);
        CHECK (markDifference.pixels == 0);
    }
}

TEST_CASE ("At a display scale of 150 %, the view starts on the screen's whole pixels however it moves, so that edges stay sharp")
{
    // Known bug 17 until 2026-11-11: the view started on the editor's whole pixels, which at 150 % are half a screen pixel
    // apart, so that notes' edges, drawn on the screen's pixels (TimelineView::beatToEdge()), were sharp in one view and soft
    // in the next (after a page turned, the song stopped, or the view scrolled).
    struct ScopedScale
    {
        explicit ScopedScale (float scale) { juce::Desktop::getInstance().setGlobalScaleFactor (scale); }
        ~ScopedScale() { juce::Desktop::getInstance().setGlobalScaleFactor (1.0f); }
    } scale (1.5f);

    PlayingSong song;
    PianoRollOn shown (song);
    auto& roll = shown.pianoRoll;
    const auto& view = roll.getView();
    const auto onScreenPixels = [] (double editorPixels)
    { return std::abs (editorPixels * 1.5 - std::round (editorPixels * 1.5)) < 1.0e-6; };
    const auto checkView = [&view, &onScreenPixels]
    {
        CHECK (onScreenPixels (view.start.inBeats() * view.pixelsPerBeat));

        for (const auto beat : { 0.37, 5.5, 9.123, 17.0 })
            CHECK (onScreenPixels (view.beatToEdge (beats (beat), 1.5f)));
    };

    // Put anywhere, or zoomed, the view is two of the editor's pixels apart from the song's start, three of the screen's.
    CHECK (roll.getViewStep() == 2);

    for (const auto beat : { 3.1, 7.77, 12.345, 31.0 })
    {
        CAPTURE (beat);
        roll.setViewStart (beats (beat));
        checkView();
    }

    roll.zoomAround (123.0, 1.37);
    checkView();

    // Scrolling continuously, the view steps by them too, the song gliding by less than a step between.
    followContinuously (roll);
    song.play();

    for (const auto part : parts)
    {
        CAPTURE (part);
        song.showFrameAt (roll, std::floor (40.0 * view.pixelsPerBeat) + part);
        REQUIRE (view.start.inBeats() > 0.0);
        CHECK (onScreenPixels (view.start.inBeats() * view.pixelsPerBeat));
        CHECK (roll.getSubPixel() >= 0.0f);
        CHECK (roll.getSubPixel() < 2.0f);
    }
}
