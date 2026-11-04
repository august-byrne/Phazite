// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Playback.h"
#include "internal/Recording.h"
#include "internal/Take.h"
#include "internal/Tempo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
namespace
{
    // Written only when it changes, so a project that never looped anything else has nothing new to save.
    void setLoopFollowsSong (Song& song, bool follows)
    {
        if (doesLoopFollowSong (song) != follows)
            song.getState().setProperty (schema::loopFollowsSong, follows, nullptr);
    }

    // The song's switches, each written only while it is on.
    void setSwitch (Song& song, const juce::Identifier& property, bool on)
    {
        if (on)
            song.getState().setProperty (property, true, nullptr);
        else
            song.getState().removeProperty (property, nullptr);
    }
} // namespace

BeatPosition getSongEnd (const Song& song)
{
    const auto lastClipEnd = getClipsEnd (song);

    if (lastClipEnd <= BeatPosition())
        return {}; // no clips: the song ends where it starts

    // To the end of the bar the last clip ends in.
    const auto bars = getBars (song);
    const auto bar = bars.getBarAt (lastClipEnd);
    const auto start = bars.getBarStart (bar);
    return lastClipEnd.inBeats() - start.inBeats() > 1.0e-6 ? bars.getBarStart (bar + 1) : start;
}

BeatRange getSongLoopBeats (const Song& song)
{
    // At least a bar long: a song without clips loops its first bar.
    const auto bars = getBars (song);
    const auto firstBarEnd = bars.getBarStart (bars.getBarAt (BeatPosition()) + 1);
    return { BeatPosition(), std::max (getSongEnd (song), firstBarEnd) };
}

bool doesLoopFollowSong (const Song& song)
{
    return song.getState().getProperty (schema::loopFollowsSong, true);
}

void loopSong (Song& song)
{
    setLoopFollowsSong (song, true);
    auto state = song.getState();
    state.removeProperty (schema::loopStart, nullptr);
    state.removeProperty (schema::loopEnd, nullptr);
    setSwitch (song, schema::loop, true);
}

void loopBeats (Song& song, BeatRange range)
{
    if (range.isEmpty())
        return;

    setLoopFollowsSong (song, false);
    auto state = song.getState();
    state.setProperty (schema::loopStart, std::max (0.0, range.getStart().inBeats()), nullptr);
    state.setProperty (schema::loopEnd, range.getEnd().inBeats(), nullptr);
    setSwitch (song, schema::loop, true);
}

BeatRange getLoopBeats (const Song& song)
{
    const auto state = song.getState();

    if (doesLoopFollowSong (song) || ! state.hasProperty (schema::loopStart) || ! state.hasProperty (schema::loopEnd))
        return getSongLoopBeats (song);

    return { BeatPosition::fromBeats (state[schema::loopStart]), BeatPosition::fromBeats (state[schema::loopEnd]) };
}

std::optional<BeatRange> getLoop (const Song& song)
{
    return isLoopOn (song) ? std::optional (getLoopBeats (song)) : std::nullopt;
}

std::optional<std::pair<double, double>> getLoopBeatRange (const Song& song)
{
    if (! isLoopOn (song))
        return {};

    const auto beats = getLoopBeats (song);
    return std::pair { beats.getStart().inBeats(), beats.getEnd().inBeats() };
}

void loopBeatRange (Song& song, double startBeat, double endBeat)
{
    if (endBeat > startBeat)
        loopBeats (song, { BeatPosition::fromBeats (std::max (0.0, startBeat)), BeatPosition::fromBeats (endBeat) });
}

bool isLoopOn (const Song& song)
{
    return song.getState().getProperty (schema::loop, false);
}

void toggleLoop (Song& song)
{
    setSwitch (song, schema::loop, ! isLoopOn (song));
}

bool isLoopingSong (const Song& song)
{
    return isLoopOn (song) && doesLoopFollowSong (song);
}

void loopSelection (Song& song, const RangeSelection& range)
{
    if (range.isEmpty())
        loopSong (song);
    else
        loopBeats (song, range.getRange());
}

void loopSections (Song& song, const std::vector<Section>& sections)
{
    if (! sections.empty())
        loopBeats (song, getSectionsRange (sections));
}

BeatPosition getPlayStartBeat (const Song& song)
{
    return BeatPosition::fromBeats ((double) song.getState().getProperty (schema::playStart, 0.0));
}

void movePlayhead (Song& song, BeatPosition position)
{
    // The song's start isn't written: a project that never played from elsewhere has nothing new to save.
    if (position.inBeats() > 0.0)
        song.getState().setProperty (schema::playStart, position.inBeats(), nullptr);
    else
        song.getState().removeProperty (schema::playStart, nullptr);

    if (auto* session = findPlayingSession (song))
        session->locate (position);
}

void goToStart (Song& song)
{
    movePlayhead (song, BeatPosition());
}

void goToEnd (Song& song)
{
    movePlayhead (song, getSongEnd (song));
}

void stopPlaying (Song& song)
{
    auto* session = findPlayingSession (song);

    if (session == nullptr)
        return;

    auto take = session->releaseTake();
    session->stop();

    // A take recorded through Phazite's engine (Take.h).
    if (take != nullptr)
    {
        const UndoStep step (song, "Record");
        take->finish();
    }
}

void playOrStop (Song& song)
{
    if (isSongPlaying (song))
    {
        stopAndReturn (song);
    }
    else if (auto* session = findPlayingSession (song))
    {
        session->locate (getPlayStartBeat (song));
        session->play();
    }
}

void playOrPause (Song& song)
{
    if (isSongPlaying (song))
        stopPlaying (song);
    else if (auto* session = findPlayingSession (song))
        session->play();
}

void stopAndReturn (Song& song)
{
    stopPlaying (song);

    if (auto* session = findPlayingSession (song))
        session->locate (getPlayStartBeat (song));
}

bool isSongPlaying (const Song& song)
{
    const auto* session = findPlayingSession (song);
    return session != nullptr && session->isPlaying();
}

BeatPosition getPlayheadBeat (const Song& song)
{
    const auto* session = findPlayingSession (song);
    return session != nullptr ? session->getPlayhead() : getPlayStartBeat (song);
}

bool isLowLatencyOn (const PlaybackHost& host)
{
    return host.isLowLatency();
}

void toggleLowLatency (PlaybackHost& host)
{
    host.setLowLatency (! host.isLowLatency());
}

//==============================================================================
void addTransportCommands (CommandRegistry& registry, Selection& selection, PlaybackHost& host)
{
    const auto hasSong = [&selection] { return selection.getSong() != nullptr; };

    // As in FL Studio and Ableton Live, stopping goes back to where playback started, so that a part can be heard
    // again and again while it is tweaked; Play / Pause goes on from where it stopped. Both work over plug-in windows.
    registry.add ({ .id = "transport.playStop",
                    .name = "Play / Stop",
                    .category = "Transport",
                    .description = "Plays from where you last put the playhead, or stops and goes back there.",
                    .synonyms = { "start", "playback", "return to start" },
                    .perform = withSong (selection, playOrStop),
                    .isEnabled = hasSong,
                    .overPlugins = true });

    registry.add ({ .id = "transport.playPause",
                    .name = "Play / Pause",
                    .category = "Transport",
                    .description = "Plays from where the playhead is, or stops it there, so that playback goes on from there.",
                    .synonyms = { "pause", "continue", "resume" },
                    .perform = withSong (selection, playOrPause),
                    .isEnabled = hasSong,
                    .overPlugins = true });

    registry.add ({ .id = "transport.goToStart",
                    .name = "Go to Start",
                    .category = "Transport",
                    .description = "Moves the playhead to the start of the song.",
                    .synonyms = { "home", "beginning", "rewind", "return" },
                    .perform = withSong (selection, goToStart),
                    .isEnabled = hasSong });

    registry.add ({ .id = "transport.goToEnd",
                    .name = "Go to End",
                    .category = "Transport",
                    .description = "Moves the playhead to the end of the song: the end of the bar its last clip ends in.",
                    .synonyms = { "end", "last", "fast forward" },
                    .perform = withSong (selection, goToEnd),
                    .isEnabled = hasSong });

    // As Cubase's Constrain Delay Compensation and Studio One's Low Latency Monitoring do (docs/design.md, section 20).
    registry.add ({ .id = "transport.lowLatency",
                    .name = "Low Latency",
                    .category = "Transport",
                    .description = "While on, what you play has no delay to wait for: devices that look ahead are bypassed, delay "
                                   "compensation is off and the buffer shrinks. Turn it off to mix.",
                    .synonyms = { "latency", "delay", "lag", "monitoring", "buffer", "delay compensation", "live" },
                    .perform = [&host] { toggleLowLatency (host); },
                    .isEnabled = hasSong,
                    .isTicked = [&host] { return isLowLatencyOn (host); } });
}

void addLoopCommands (CommandRegistry& registry, Selection& selection, RangeSelection& range)
{
    const auto hasSong = [&selection] { return selection.getSong() != nullptr; };

    registry.add ({ .id = "transport.loop",
                    .name = "Loop",
                    .category = "Transport",
                    .description = "Plays the song over and over, or the part looped on purpose: at its end, playback starts again.",
                    .synonyms = { "repeat", "cycle", "loop playback" },
                    .perform = withSong (selection, toggleLoop),
                    .isEnabled = hasSong,
                    .isTicked = ifSong (selection, isLoopOn) });

    registry.add ({ .id = "transport.loopSelection",
                    .name = "Loop Selection",
                    .category = "Transport",
                    .description = "Loops the time range selected in the playlist, or the whole song when none is selected.",
                    .synonyms = { "loop range", "cycle region", "loop brace", "set loop" },
                    .perform = withSong (selection, [&range] (Song& song) { loopSelection (song, range); }),
                    .isEnabled = hasSong });

    registry.add ({ .id = "section.loop",
                    .name = "Loop Section",
                    .category = "Section",
                    .description = "Loops the selected sections instead of the whole song.",
                    .synonyms = { "repeat section", "cycle", "practice" },
                    .perform = withSong (selection, [&selection] (Song& song) { loopSections (song, getSelectedSections (selection)); }),
                    .isEnabled = [&selection] { return hasSelectedSections (selection); } });

    registry.add (
        { .id = "transport.loopSong",
          .name = "Loop the Song",
          .category = "Transport",
          .description =
              "Loops the whole song again, from its start to the end of the bar its last clip ends in; the loop grows as clips go later.",
          .synonyms = { "loop all", "whole song", "song mode" },
          .perform = withSong (selection, loopSong),
          .isEnabled = hasSong,
          .isTicked = ifSong (selection, isLoopingSong) });
}
} // namespace phazite
