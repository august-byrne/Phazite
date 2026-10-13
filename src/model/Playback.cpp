// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Playback.h"
#include "Project.h"
#include "Sections.h"

namespace phazite
{
namespace
{
    const juce::Identifier loopFollowsSongProperty { "loopFollowsSong" };

    // Written only when it changes, so a project that never looped anything else has nothing new to save.
    void setLoopFollowsSong (te::Edit& edit, bool follows)
    {
        if (doesLoopFollowSong (edit) != follows)
            getPhaziteState (edit).setProperty (loopFollowsSongProperty, follows, nullptr);
    }

    bool isNear (te::TimePosition a, te::TimePosition b)
    {
        return std::abs ((a - b).inSeconds()) < 1.0e-6;
    }
} // namespace

te::TimeRange getSongLoopRange (const te::Edit& edit)
{
    auto end = te::TimePosition();

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            end = std::max (end, clip->getPosition().getEnd());

    // To the end of the bar the last clip ends in.
    const auto& tempo = edit.tempoSequence;
    const auto barsAndBeats = tempo.toBarsAndBeats (end);
    const auto bars = barsAndBeats.bars + (barsAndBeats.beats.inBeats() > 1.0e-6 ? 1 : 0);
    return { te::TimePosition(), tempo.toTime (te::tempo::BarsAndBeats { std::max (1, bars), {}, 0 }) };
}

bool doesLoopFollowSong (const te::Edit& edit)
{
    return findPhaziteState (edit).getProperty (loopFollowsSongProperty, true);
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
    if (! doesLoopFollowSong (edit))
        return;

    auto& transport = edit.getTransport();
    const auto song = getSongLoopRange (edit);

    if (const auto loop = transport.getLoopRange(); ! isNear (loop.getStart(), song.getStart()) || ! isNear (loop.getEnd(), song.getEnd()))
        transport.setLoopRange (song);
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
        transport.stop (false, false);
    else
        transport.play (false);
}

void stopAndReturn (te::Edit& edit)
{
    auto& transport = edit.getTransport();
    transport.stop (false, false);
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
void addLoopCommands (CommandRegistry& registry, te::SelectionManager& selection, RangeSelection& range)
{
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "transport.loop",
                    .name = "Loop",
                    .category = "Transport",
                    .description = "Plays the song over and over, or the part looped on purpose: at its end, playback starts again.",
                    .synonyms = { "repeat", "cycle", "loop playback" },
                    .perform =
                        [&selection]
                    {
                        if (auto* edit = selection.getEdit())
                        {
                            auto& transport = edit->getTransport();
                            transport.looping = ! transport.looping.get();
                            updateSongLoop (*edit);
                        }
                    },
                    .isEnabled = hasEdit,
                    .isTicked =
                        [&selection]
                    {
                        auto* edit = selection.getEdit();
                        return edit != nullptr && edit->getTransport().looping.get();
                    } });

    registry.add ({ .id = "transport.loopSelection",
                    .name = "Loop Selection",
                    .category = "Transport",
                    .description = "Loops the time range selected in the playlist, or the whole song when none is selected.",
                    .synonyms = { "loop range", "cycle region", "loop brace", "set loop" },
                    .perform =
                        [&selection, &range]
                    {
                        if (auto* edit = selection.getEdit())
                        {
                            if (range.isEmpty())
                                loopSong (*edit);
                            else
                                loopRange (*edit, edit->tempoSequence.toTime (range.getRange()));
                        }
                    },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "section.loop",
                    .name = "Loop Section",
                    .category = "Section",
                    .description = "Loops the selected sections instead of the whole song.",
                    .synonyms = { "repeat section", "cycle", "practice" },
                    .perform =
                        [&selection]
                    {
                        const auto sections = getSelectedSections (selection);

                        if (sections.isEmpty())
                            return;

                        auto loop = sections.getFirst()->getEditTimeRange();

                        for (auto* section : sections)
                            loop = loop.getUnionWith (section->getEditTimeRange());

                        loopRange (sections.getFirst()->edit, loop);
                    },
                    .isEnabled = [&selection] { return ! getSelectedSections (selection).isEmpty(); } });

    registry.add (
        { .id = "transport.loopSong",
          .name = "Loop the Song",
          .category = "Transport",
          .description = "Loops the whole song again, from its start to the end of its last clip; the loop grows as clips go later.",
          .synonyms = { "loop all", "whole song", "song mode" },
          .perform =
              [&selection]
          {
              if (auto* edit = selection.getEdit())
                  loopSong (*edit);
          },
          .isEnabled = hasEdit,
          .isTicked =
              [&selection]
          {
              auto* edit = selection.getEdit();
              return edit != nullptr && edit->getTransport().looping.get() && doesLoopFollowSong (*edit);
          } });
}
} // namespace phazite
