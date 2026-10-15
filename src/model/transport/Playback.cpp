// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Playback.h"
#include "model/arrangement/Sections.h"
#include "model/base/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/Undo.h"

namespace phazite
{
namespace
{
    // Written only when it changes, so a project that never looped anything else has nothing new to save.
    void setLoopFollowsSong (te::Edit& edit, bool follows)
    {
        if (doesLoopFollowSong (edit) != follows)
            getPhaziteState (edit).setProperty (schema::loopFollowsSong, follows, nullptr);
    }

    bool isNear (te::TimePosition a, te::TimePosition b)
    {
        return std::abs ((a - b).inSeconds()) < 1.0e-6;
    }

    // Stops playback. A recording that stops with it is kept as one undo step, "Record" (Recording.h).
    void stopPlaying (te::Edit& edit)
    {
        auto& transport = edit.getTransport();

        if (transport.isRecording())
        {
            {
                const UndoStep step (edit, "Record");
                transport.stop (false, false);
            }

            updateSongLoop (edit); // which the recording set aside
            return;
        }

        transport.stop (false, false);
    }
} // namespace

te::TimePosition getSongEnd (const te::Edit& edit)
{
    auto lastClipEnd = te::TimePosition();

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            lastClipEnd = std::max (lastClipEnd, clip->getPosition().getEnd());

    if (lastClipEnd <= te::TimePosition())
        return {}; // no clips: the song ends where it starts

    // To the end of the bar the last clip ends in.
    const auto& tempo = edit.tempoSequence;
    const auto barsAndBeats = tempo.toBarsAndBeats (lastClipEnd);
    const auto bars = barsAndBeats.bars + (barsAndBeats.beats.inBeats() > 1.0e-6 ? 1 : 0);
    return tempo.toTime (te::tempo::BarsAndBeats { bars, {}, 0 });
}

te::TimeRange getSongLoopRange (const te::Edit& edit)
{
    // At least a bar long: a song without clips loops its first bar.
    const auto firstBarEnd = edit.tempoSequence.toTime (te::tempo::BarsAndBeats { 1, {}, 0 });
    return { te::TimePosition(), std::max (getSongEnd (edit), firstBarEnd) };
}

bool doesLoopFollowSong (const te::Edit& edit)
{
    return findPhaziteState (edit).getProperty (schema::loopFollowsSong, true);
}

void loopSong (te::Edit& edit)
{
    setLoopFollowsSong (edit, true);
    edit.getTransport().looping = true;
    updateSongLoop (edit);
}

void loopRange (te::Edit& edit, te::TimeRange range)
{
    if (range.getLength() <= te::TimeDuration())
        return;

    setLoopFollowsSong (edit, false);
    auto& transport = edit.getTransport();
    transport.setLoopRange (range);
    transport.looping = true;
}

void updateSongLoop (te::Edit& edit)
{
    // Not during a recording, which runs on past the song's end (Recording.h).
    if (! doesLoopFollowSong (edit) || edit.getTransport().isRecording())
        return;

    auto& transport = edit.getTransport();
    const auto song = getSongLoopRange (edit);

    if (const auto loop = transport.getLoopRange(); ! isNear (loop.getStart(), song.getStart()) || ! isNear (loop.getEnd(), song.getEnd()))
        transport.setLoopRange (song);
}

bool isLoopOn (const te::Edit& edit)
{
    return edit.getTransport().looping.get();
}

void toggleLoop (te::Edit& edit)
{
    auto& transport = edit.getTransport();
    transport.looping = ! transport.looping.get();
    updateSongLoop (edit);
}

bool isLoopingSong (const te::Edit& edit)
{
    return isLoopOn (edit) && doesLoopFollowSong (edit);
}

void loopSelection (te::Edit& edit, const RangeSelection& range)
{
    if (range.isEmpty())
        loopSong (edit);
    else
        loopRange (edit, edit.tempoSequence.toTime (range.getRange()));
}

void loopSections (const juce::Array<te::ArrangerClip*>& sections)
{
    if (! sections.isEmpty())
        loopRange (sections.getFirst()->edit, getSectionsTimeRange (sections));
}

te::TimePosition getPlayStart (const te::Edit& edit)
{
    return edit.getTransport().startPosition.get();
}

void movePlayhead (te::Edit& edit, te::TimePosition position)
{
    auto& transport = edit.getTransport();
    transport.startPosition = position;
    transport.setPosition (position);
}

void goToStart (te::Edit& edit)
{
    movePlayhead (edit, te::TimePosition());
}

void goToEnd (te::Edit& edit)
{
    movePlayhead (edit, getSongEnd (edit));
}

void playOrStop (te::Edit& edit)
{
    auto& transport = edit.getTransport();

    if (transport.isPlaying())
        stopAndReturn (edit);
    else
        transport.playFromStart (false);
}

void playOrPause (te::Edit& edit)
{
    auto& transport = edit.getTransport();

    if (transport.isPlaying())
        stopPlaying (edit);
    else
        transport.play (false);
}

void stopAndReturn (te::Edit& edit)
{
    stopPlaying (edit);
    auto& transport = edit.getTransport();
    transport.setPosition (transport.startPosition);
}

//==============================================================================
SongFollower::SongFollower (te::Edit& e)
    : edit (e),
      editState (e.state)
{
    editState.addListener (this);
    handleAsyncUpdate(); // a project saved with another loop around its song gets the song's again
    startTimerHz (30);
}

SongFollower::~SongFollower()
{
    stopTimer();
    cancelPendingUpdate();
    editState.removeListener (this);
}

void SongFollower::handleAsyncUpdate()
{
    songEnd = getSongLoopRange (edit).getEnd();
    updateSongLoop (edit);
}

void SongFollower::timerCallback()
{
    auto& transport = edit.getTransport();

    // A recording that stopped some other way than Playback.h's functions: the song's loop back.
    if (const auto recording = transport.isRecording(); recording != wasRecording)
    {
        wasRecording = recording;

        if (! recording)
            updateSongLoop (edit);
    }

    if (! transport.isPlaying() || transport.looping.get() || transport.isRecording())
    {
        lastPosition.reset();
        return;
    }

    // Only when the playhead crosses the end: playback started past it plays on, there being nothing to cut short.
    const auto position = transport.getPosition();

    if (lastPosition.has_value() && *lastPosition < songEnd && position >= songEnd)
    {
        stopAndReturn (edit);
        lastPosition.reset();
        return;
    }

    lastPosition = position;
}

void SongFollower::changed (const juce::ValueTree& tree)
{
    // Clips (where they are and how long), tracks (with their clips) and the tempo (where bars end). Not the
    // transport, which the loop itself is in.
    for (auto t = tree; t.isValid(); t = t.getParent())
    {
        if (t.hasType (te::IDs::TRANSPORT))
            return;

        if (te::Clip::isClipState (t) || t.hasType (te::IDs::TRACK) || t.hasType (te::IDs::TEMPOSEQUENCE))
        {
            triggerAsyncUpdate();
            return;
        }
    }
}

//==============================================================================
void addTransportCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    // As in FL Studio and Ableton Live, stopping goes back to where playback started, so that a part can be heard
    // again and again while it is tweaked; Play / Pause goes on from where it stopped. Both work over plug-in windows.
    registry.add ({ .id = "transport.playStop",
                    .name = "Play / Stop",
                    .category = "Transport",
                    .description = "Plays from where you last put the playhead, or stops and goes back there.",
                    .synonyms = { "start", "playback", "return to start" },
                    .perform = withEdit (selection, playOrStop),
                    .isEnabled = hasEdit,
                    .overPlugins = true });

    registry.add ({ .id = "transport.playPause",
                    .name = "Play / Pause",
                    .category = "Transport",
                    .description = "Plays from where the playhead is, or stops it there, so that playback goes on from there.",
                    .synonyms = { "pause", "continue", "resume" },
                    .perform = withEdit (selection, playOrPause),
                    .isEnabled = hasEdit,
                    .overPlugins = true });

    registry.add ({ .id = "transport.goToStart",
                    .name = "Go to Start",
                    .category = "Transport",
                    .description = "Moves the playhead to the start of the song.",
                    .synonyms = { "home", "beginning", "rewind", "return" },
                    .perform = withEdit (selection, goToStart),
                    .isEnabled = hasEdit });

    registry.add ({ .id = "transport.goToEnd",
                    .name = "Go to End",
                    .category = "Transport",
                    .description = "Moves the playhead to the end of the song: the end of the bar its last clip ends in.",
                    .synonyms = { "end", "last", "fast forward" },
                    .perform = withEdit (selection, goToEnd),
                    .isEnabled = hasEdit });
}

void addLoopCommands (CommandRegistry& registry, te::SelectionManager& selection, RangeSelection& range)
{
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "transport.loop",
                    .name = "Loop",
                    .category = "Transport",
                    .description = "Plays the song over and over, or the part looped on purpose: at its end, playback starts again.",
                    .synonyms = { "repeat", "cycle", "loop playback" },
                    .perform = withEdit (selection, toggleLoop),
                    .isEnabled = hasEdit,
                    .isTicked = ifEdit (selection, isLoopOn) });

    registry.add ({ .id = "transport.loopSelection",
                    .name = "Loop Selection",
                    .category = "Transport",
                    .description = "Loops the time range selected in the playlist, or the whole song when none is selected.",
                    .synonyms = { "loop range", "cycle region", "loop brace", "set loop" },
                    .perform = withEdit (selection, [&range] (te::Edit& edit) { loopSelection (edit, range); }),
                    .isEnabled = hasEdit });

    registry.add ({ .id = "section.loop",
                    .name = "Loop Section",
                    .category = "Section",
                    .description = "Loops the selected sections instead of the whole song.",
                    .synonyms = { "repeat section", "cycle", "practice" },
                    .perform = [&selection] { loopSections (getSelectedSections (selection)); },
                    .isEnabled = [&selection] { return hasSelectedSections (selection); } });

    registry.add (
        { .id = "transport.loopSong",
          .name = "Loop the Song",
          .category = "Transport",
          .description =
              "Loops the whole song again, from its start to the end of the bar its last clip ends in; the loop grows as clips go later.",
          .synonyms = { "loop all", "whole song", "song mode" },
          .perform = withEdit (selection, loopSong),
          .isEnabled = hasEdit,
          .isTicked = ifEdit (selection, isLoopingSong) });
}
} // namespace phazite
