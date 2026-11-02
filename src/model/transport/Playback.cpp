// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Playback.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mirror/internal/Mirror.h"
#include "model/playback/internal/PlaybackSession.h"

namespace phazite
{
namespace
{
    // Where playback starts, in the song's beats.
    BeatPosition getPlayStartBeat (const te::Edit& edit)
    {
        return BeatPosition::fromBeats ((double) findPhaziteState (edit).getProperty (schema::playStart, 0.0));
    }

    // Written only when it changes, so a project that never looped anything else has nothing new to save.
    void setLoopFollowsSong (te::Edit& edit, bool follows)
    {
        if (doesLoopFollowSong (edit) != follows)
            getPhaziteState (edit).setProperty (schema::loopFollowsSong, follows, nullptr);
    }

    // The song's switches, each written only while it is on.
    void setSwitch (te::Edit& edit, const juce::Identifier& property, bool on)
    {
        if (on)
            getPhaziteState (edit).setProperty (property, true, nullptr);
        else if (auto state = findPhaziteState (edit); state.isValid())
            state.removeProperty (property, nullptr);
    }

    // Loops a range of the song on purpose.
    void loopBeats (te::Edit& edit, BeatRange range)
    {
        if (range.isEmpty())
            return;

        setLoopFollowsSong (edit, false);
        auto state = getPhaziteState (edit);
        state.setProperty (schema::loopStart, std::max (0.0, range.getStart().inBeats()), nullptr);
        state.setProperty (schema::loopEnd, range.getEnd().inBeats(), nullptr);
        setSwitch (edit, schema::loop, true);
        mirror::reconcileTransport (edit);
    }

    // Stops playback. A recording that stops with it is kept as one undo step, "Record" (Recording.h).
    void stopPlaying (te::Edit& edit)
    {
        if (auto* session = findPlayingSession (edit))
        {
            session->stop();
            return;
        }

        auto& transport = edit.getTransport();

        if (transport.isRecording())
        {
            {
                const UndoStep step (edit, "Record");
                transport.stop (false, false);
                adoptRecordedMidi (edit); // what Tracktion recorded becomes the song's
            }

            // A punched take turned Tracktion's loop off, and a take around the song's loop ran on past its end
            // (Recording.h): the song's loop again.
            edit.recordingPunchInOut = false;
            mirror::reconcileTransport (edit);
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

    if (auto state = findPhaziteState (edit); state.isValid())
    {
        state.removeProperty (schema::loopStart, nullptr);
        state.removeProperty (schema::loopEnd, nullptr);
    }

    setSwitch (edit, schema::loop, true);
    mirror::reconcileTransport (edit);
}

void loopRange (te::Edit& edit, te::TimeRange range)
{
    loopBeats (edit, fromTracktion (edit.tempoSequence.toBeats (range)));
}

BeatRange getLoopBeats (const te::Edit& edit)
{
    const auto state = findPhaziteState (edit);

    if (doesLoopFollowSong (edit) || ! state.hasProperty (schema::loopStart) || ! state.hasProperty (schema::loopEnd))
        return fromTracktion (edit.tempoSequence.toBeats (getSongLoopRange (edit)));

    return { BeatPosition::fromBeats (state[schema::loopStart]), BeatPosition::fromBeats (state[schema::loopEnd]) };
}

std::optional<std::pair<double, double>> getLoopBeatRange (const te::Edit& edit)
{
    if (! isLoopOn (edit))
        return {};

    const auto beats = getLoopBeats (edit);
    return std::pair { beats.getStart().inBeats(), beats.getEnd().inBeats() };
}

void loopBeatRange (te::Edit& edit, double startBeat, double endBeat)
{
    if (endBeat > startBeat)
        loopBeats (edit, { BeatPosition::fromBeats (std::max (0.0, startBeat)), BeatPosition::fromBeats (endBeat) });
}

bool isLoopOn (const te::Edit& edit)
{
    return findPhaziteState (edit).getProperty (schema::loop, false);
}

void toggleLoop (te::Edit& edit)
{
    setSwitch (edit, schema::loop, ! isLoopOn (edit));
    mirror::reconcileTransport (edit);
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
        loopBeats (edit, fromTracktion (range.getRange()));
}

void loopSections (te::Edit& edit, const std::vector<Section>& sections)
{
    if (! sections.empty())
        loopBeats (edit, getSectionsRange (sections));
}

te::TimePosition getPlayStart (const te::Edit& edit)
{
    return edit.tempoSequence.toTime (toTracktion (getPlayStartBeat (edit)));
}

void movePlayhead (te::Edit& edit, te::TimePosition position)
{
    // The song's start isn't written: a project that never played from elsewhere has nothing new to save.
    if (const auto beat = edit.tempoSequence.toBeats (position).inBeats(); beat > 0.0)
        getPhaziteState (edit).setProperty (schema::playStart, beat, nullptr);
    else if (auto state = findPhaziteState (edit); state.isValid())
        state.removeProperty (schema::playStart, nullptr);

    mirror::reconcileTransport (edit);

    if (auto* session = findPlayingSession (edit))
        session->locate (fromTracktion (edit.tempoSequence.toBeats (position)));
    else
        edit.getTransport().setPosition (position);
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
    if (isSongPlaying (edit))
    {
        stopAndReturn (edit);
    }
    else if (auto* session = findPlayingSession (edit))
    {
        session->locate (getPlayStartBeat (edit));
        session->play();
    }
    else
    {
        edit.getTransport().playFromStart (false);
    }
}

void playOrPause (te::Edit& edit)
{
    if (isSongPlaying (edit))
        stopPlaying (edit);
    else if (auto* session = findPlayingSession (edit))
        session->play();
    else
        edit.getTransport().play (false);
}

void stopAndReturn (te::Edit& edit)
{
    stopPlaying (edit);

    if (auto* session = findPlayingSession (edit))
    {
        session->locate (getPlayStartBeat (edit));
        return;
    }

    auto& transport = edit.getTransport();
    const auto start = getPlayStart (edit);
    const auto alreadyThere = transport.getPosition() == start;
    transport.setPosition (start);

    // For 200 ms after playback starts, Tracktion's position still reads as where it started, so setting it there again
    // changes nothing, and the playhead would stay where it stopped: the change is sent anyway.
    if (alreadyThere)
        transport.state.sendPropertyChangeMessage (te::IDs::position);
}

bool isSongPlaying (const te::Edit& edit)
{
    if (const auto* session = findPlayingSession (edit))
        return session->isPlaying();

    return edit.getTransport().isPlaying();
}

te::TimePosition getPlayheadTime (const te::Edit& edit)
{
    if (const auto* session = findPlayingSession (edit))
        return edit.tempoSequence.toTime (toTracktion (session->getPlayhead()));

    return edit.getTransport().getPosition();
}

bool isLowLatencyOn (const te::Edit& edit)
{
    return isLowLatency (edit.engine);
}

void toggleLowLatency (te::Edit& edit)
{
    setLowLatency (edit.engine, ! isLowLatency (edit.engine));
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
    mirror::reconcileTransport (edit); // the song's loop around it
}

void SongFollower::timerCallback()
{
    auto& transport = edit.getTransport();

    // A recording that stopped some other way than Playback.h's functions: the song's loop back.
    if (const auto recording = transport.isRecording(); recording != wasRecording)
    {
        wasRecording = recording;

        if (! recording)
            mirror::reconcileTransport (edit);

        // Tracktion plays live MIDI while it records, and Phazite's own engine again after (T2 step 9 changes that).
        if (auto* session = findSession (edit))
            session->followLiveMidi();
    }

    if (! isSongPlaying (edit) || isLoopOn (edit) || transport.isRecording())
    {
        lastPosition.reset();
        return;
    }

    // Only when the playhead crosses the end: playback started past it plays on, there being nothing to cut short.
    const auto position = getPlayheadTime (edit);

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
void addTransportCommands (CommandRegistry& registry, Selection& selection)
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

    // As Cubase's Constrain Delay Compensation and Studio One's Low Latency Monitoring do (docs/design.md, section 20).
    registry.add ({ .id = "transport.lowLatency",
                    .name = "Low Latency",
                    .category = "Transport",
                    .description = "While on, what you play has no delay to wait for: devices that look ahead are bypassed, delay "
                                   "compensation is off and the buffer shrinks. Turn it off to mix.",
                    .synonyms = { "latency", "delay", "lag", "monitoring", "buffer", "delay compensation", "live" },
                    .perform = withEdit (selection, toggleLowLatency),
                    .isEnabled = hasEdit,
                    .isTicked = ifEdit (selection, isLowLatencyOn) });
}

void addLoopCommands (CommandRegistry& registry, Selection& selection, RangeSelection& range)
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

    registry.add (
        { .id = "section.loop",
          .name = "Loop Section",
          .category = "Section",
          .description = "Loops the selected sections instead of the whole song.",
          .synonyms = { "repeat section", "cycle", "practice" },
          .perform = withEdit (selection, [&selection] (te::Edit& edit) { loopSections (edit, getSelectedSections (selection)); }),
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
