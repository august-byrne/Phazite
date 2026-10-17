// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation clips (src/model/automation/AutomationModel.h: createAutomationClip(); roadmap step 2.2): made for a
// parameter, they set it while they play and leave it to its own value outside them; aliases share their curve; they
// move only to tracks with their kind of device; playing them is no unsaved change, and a save in their middle keeps the
// value set by hand. Then in the playlist, driven with the mouse: the curve on the clip and its points.
#include "GoldenFiles.h"
#include "RealTimePlayback.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/DesignTokens.h"

#include <cmath>

namespace
{
using phazite::BeatPosition;
using phazite::ClipID;
using phazite::CurveRef;
using phazite::ParameterRef;

BeatPosition beats (double b)
{
    return BeatPosition::fromBeats (b);
}

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

std::vector<double> beatsOf (const std::vector<phazite::AutomationPoint>& points)
{
    std::vector<double> result;

    for (const auto& point : points)
        result.push_back (point.beat.inBeats());

    return result;
}

// An open project with two tracks, A (with a synth, then a compressor) and B, and no undo history.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        a->setName ("A");
        b->setName ("B");
        setEdit (context, edit.get());
        phazite::chooseInstrument (context, idOf (*a), find ("phaziteSynth"));
        phazite::addEffect (context, idOf (*a), find ("phaziteCompressor"));
        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    // A track's compressor's threshold, from -60 to 0 dB.
    ParameterRef threshold (const te::Track& track) const
    {
        const auto effects = phazite::getEffects (context, idOf (track));
        REQUIRE (effects.size() == 1);
        return { effects[0].id, "threshold" };
    }

    ParameterRef threshold() const { return threshold (*a); }

    // The value the engine sets a parameter to at a beat.
    float valueAt (const ParameterRef& ref, double beat) const
    {
        const auto parameter = phazite::findParameter (*edit, ref);
        REQUIRE (parameter != nullptr);
        parameter->updateToFollowCurve (edit->tempoSequence.toTime (te::BeatPosition::fromBeats (beat)));
        return parameter->getCurrentValue();
    }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
};
} // namespace

TEST_CASE ("An automation clip sets its parameter while it plays, and the parameter's own value before and after it")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    phazite::getParameter (context, threshold).setValue (-20.0f);
    song.edit->getUndoManager().clearUndoHistory();

    const auto clip = phazite::createAutomationClip (context, threshold, { beats (4), beats (8) });
    REQUIRE (clip.isValid());
    CHECK (song.describeUndo() == juce::String ("Undo Create Automation Clip"));

    // On the parameter's track, named after it, a flat line at its value.
    const auto info = phazite::getClip (context, clip);
    REQUIRE (info.has_value());
    CHECK (info->kind == phazite::ClipType::automation);
    CHECK (info->automates == threshold);
    CHECK (info->track == idOf (*song.a));
    CHECK (info->name == juce::String ("Compressor: Threshold"));
    CHECK (info->range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (info->range.getEnd().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::getAutomationClipParameter (context, clip) == threshold);

    const CurveRef curve (threshold, clip);
    auto points = phazite::getAutomationPoints (context, curve);
    CHECK (beatsOf (points) == std::vector { 4.0, 8.0 });
    CHECK (points[0].value == doctest::Approx (-20.0f));

    // Its curve is in the song's beats, as a lane's is, and sets the parameter only while the clip plays.
    phazite::replaceAutomationPoints (context, curve, { beats (4), beats (8) }, { { beats (4), -40.0f }, { beats (8), -10.0f } });
    CHECK (song.describeUndo() == juce::String ("Undo Draw Automation"));
    CHECK (song.valueAt (threshold, 6.0) == doctest::Approx (-25.0f));
    CHECK (song.valueAt (threshold, 2.0) == doctest::Approx (-20.0f));
    CHECK (song.valueAt (threshold, 10.0) == doctest::Approx (-20.0f));

    // It moves with its curve.
    phazite::moveClip (context, clip, beats (8), idOf (*song.a));
    CHECK (beatsOf (phazite::getAutomationPoints (context, curve)) == std::vector { 8.0, 12.0 });
    CHECK (song.valueAt (threshold, 10.0) == doctest::Approx (-25.0f));

    // Other clips are no automation clips.
    const auto midi = phazite::addMidiClipAt (context, idOf (*song.a), beats (16));
    CHECK (phazite::getClip (context, midi)->kind == phazite::ClipType::midi);
    CHECK_FALSE (phazite::getAutomationClipParameter (context, midi).has_value());

    phazite::undo (*song.edit); // the MIDI clip
    phazite::undo (*song.edit); // the move
    phazite::undo (*song.edit); // the drawing
    phazite::undo (*song.edit);
    CHECK_FALSE (phazite::getClip (context, clip).has_value());
}

TEST_CASE ("A control's menu makes an automation clip four bars long from the bar the playhead is in")
{
    Song song;
    auto& context = song.context;
    song.edit->getTransport().setPosition (song.edit->tempoSequence.toTime (te::BeatPosition::fromBeats (6.0)));

    const auto clip = phazite::createAutomationClip (phazite::getParameter (context, song.threshold()));
    const auto info = phazite::getClip (context, clip);
    REQUIRE (info.has_value());
    CHECK (info->range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (info->range.getEnd().inBeats() == doctest::Approx (20.0));
    CHECK (info->track == idOf (*song.a));

    // The master's parameters' go on a track called Automation at the end, made once.
    const auto master = phazite::getAutomationTargets (context, phazite::getMasterTrack (context));
    REQUIRE (master.size() >= 2);
    const auto volumeClip = phazite::createAutomationClip (context, master[0].parameter, { beats (0), beats (4) });
    const auto panClip = phazite::createAutomationClip (context, master[1].parameter, { beats (4), beats (8) });
    REQUIRE (volumeClip.isValid());
    REQUIRE (panClip.isValid());

    const auto tracks = te::getAudioTracks (*song.edit);
    REQUIRE (tracks.size() == 3);
    CHECK (tracks[2]->getName() == juce::String ("Automation"));
    CHECK (phazite::getClip (context, volumeClip)->track == idOf (*tracks[2]));
    CHECK (phazite::getClip (context, panClip)->track == idOf (*tracks[2]));
    CHECK (phazite::getClip (context, volumeClip)->automates == master[0].parameter);
    CHECK (song.describeUndo() == juce::String ("Undo Create Automation Clip"));

    // One step each, the track with the first.
    phazite::undo (*song.edit);
    phazite::undo (*song.edit);
    CHECK (te::getAudioTracks (*song.edit).size() == 2);
}

TEST_CASE ("Create Automation Clip makes one for the parameter touched last")
{
    Song song;
    auto& context = song.context;
    phazite::CommandRegistry registry;
    phazite::addAutomationCommands (registry, context);
    const auto* command = registry.find ("automation.createClip");
    REQUIRE (command != nullptr);
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*command));
    CHECK (command->overPlugins);

    phazite::getParameter (context, song.threshold()).setValue (-12.0f);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getCurrentName (*command) == juce::String ("Create Automation Clip for Threshold"));
    REQUIRE (registry.invoke ("automation.createClip"));

    const auto clips = phazite::getClips (context, idOf (*song.a));
    REQUIRE (clips.size() == 1);
    CHECK (clips[0].kind == phazite::ClipType::automation);
    CHECK (clips[0].automates == song.threshold());
    CHECK (song.describeUndo() == juce::String ("Undo Create Automation Clip"));
}

TEST_CASE ("An automation clip's aliases share its curve; its copies have their own")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    const auto clip = phazite::createAutomationClip (context, threshold, { beats (0), beats (4) });
    const CurveRef curve (threshold, clip);
    phazite::replaceAutomationPoints (context, curve, { beats (0), beats (4) }, { { beats (0), -40.0f }, { beats (4), -10.0f } });

    const auto aliases = phazite::copyClips (context, { clip }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (context, aliases[0], beats (8), idOf (*song.a));
    const CurveRef aliasCurve (threshold, aliases[0]);
    CHECK (phazite::getClip (context, aliases[0])->kind == phazite::ClipType::automation);
    CHECK (phazite::getClip (context, aliases[0])->linked);
    CHECK (beatsOf (phazite::getAutomationPoints (context, aliasCurve)) == std::vector { 8.0, 12.0 });

    // Both play their curve where they are.
    CHECK (song.valueAt (threshold, 2.0) == doctest::Approx (-25.0f));
    CHECK (song.valueAt (threshold, 10.0) == doctest::Approx (-25.0f));

    // An edit of either is the other's too, in the same step.
    phazite::addAutomationPoint (context, curve, { beats (2), -60.0f });
    CHECK (beatsOf (phazite::getAutomationPoints (context, aliasCurve)) == std::vector { 8.0, 10.0, 12.0 });
    CHECK (song.valueAt (threshold, 10.0) == doctest::Approx (-60.0f));

    phazite::deleteAutomationPoints (context, aliasCurve, { 0 });
    CHECK (beatsOf (phazite::getAutomationPoints (context, curve)) == std::vector { 2.0, 4.0 });

    phazite::undo (*song.edit);
    CHECK (beatsOf (phazite::getAutomationPoints (context, curve)) == std::vector { 0.0, 2.0, 4.0 });
    CHECK (beatsOf (phazite::getAutomationPoints (context, aliasCurve)) == std::vector { 8.0, 10.0, 12.0 });

    // A copy's curve is its own.
    const auto copies = phazite::copyClips (context, { clip }, false);
    REQUIRE (copies.size() == 1);
    phazite::moveClip (context, copies[0], beats (16), idOf (*song.a));
    const CurveRef copyCurve (threshold, copies[0]);
    CHECK_FALSE (phazite::getClip (context, copies[0])->linked);
    phazite::deleteAutomationPoints (context, copyCurve, { 1 });
    CHECK (beatsOf (phazite::getAutomationPoints (context, copyCurve)) == std::vector { 16.0, 20.0 });
    CHECK (beatsOf (phazite::getAutomationPoints (context, curve)) == std::vector { 0.0, 2.0, 4.0 });
    CHECK (song.valueAt (threshold, 18.0) == doctest::Approx (-25.0f));
}

TEST_CASE ("An automation clip sets the parameter it was made for on any track, copied, split or moved")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a), b = idOf (*song.b);

    // Two compressors on A: the clip is the second's, which Tracktion would swap for the first on a copy.
    phazite::addEffect (context, a, song.find ("phaziteCompressor"));
    const auto effects = phazite::getEffects (context, a);
    REQUIRE (effects.size() == 2);
    const ParameterRef second { effects[1].id, "threshold" };
    const auto clip = phazite::createAutomationClip (context, second, { beats (0), beats (4) });
    phazite::replaceAutomationPoints (context, CurveRef (second, clip), { beats (0), beats (4) },
                                      { { beats (0), -50.0f }, { beats (4), -50.0f } });
    const auto automates = [&context] (ClipID id) { return phazite::getClip (context, id)->automates; };

    const auto copies = phazite::copyClips (context, { clip }, false);
    const auto aliases = phazite::copyClips (context, { clip }, true);
    const auto parts = phazite::splitClips (context, { clip }, beats (2));
    REQUIRE (copies.size() == 1);
    REQUIRE (aliases.size() == 1);
    REQUIRE (parts.size() == 1);
    CHECK (automates (copies[0]) == second);
    CHECK (automates (aliases[0]) == second);
    CHECK (automates (parts[0]) == second);
    CHECK (automates (clip) == second);

    // To B, which has no compressor, and back.
    phazite::moveClip (context, copies[0], beats (8), b);
    CHECK (phazite::getClip (context, copies[0])->track == b);
    CHECK (automates (copies[0]) == second);
    CHECK (song.valueAt (second, 9.0) == doctest::Approx (-50.0f));
    {
        const phazite::UndoStep step (*song.edit, "Move Clip");
        phazite::moveClip (context, copies[0], beats (8), a);
    }

    CHECK (automates (copies[0]) == second);

    // Undo takes the move back, with what it set.
    phazite::undo (*song.edit);
    CHECK (phazite::getClip (context, copies[0])->track == b);
    CHECK (automates (copies[0]) == second);

    // The master's volume, which is on no track, too.
    const auto master = phazite::getAutomationTargets (context, phazite::getMasterTrack (context));
    REQUIRE (! master.empty());
    const auto masterClip = phazite::createAutomationClip (context, master[0].parameter, { beats (0), beats (4) });
    phazite::replaceAutomationPoints (context, CurveRef (master[0].parameter, masterClip), { beats (0), beats (4) },
                                      { { beats (0), 0.3f }, { beats (4), 0.3f } });
    const auto masterCopies = phazite::copyClips (context, { masterClip }, true);
    REQUIRE (masterCopies.size() == 1);
    CHECK (automates (masterCopies[0]) == master[0].parameter);
    phazite::moveClip (context, masterCopies[0], beats (8), a);
    CHECK (automates (masterCopies[0]) == master[0].parameter);
    CHECK (song.valueAt (master[0].parameter, 9.0) == doctest::Approx (0.3f));

    // Draw places aliases of the clip used last, on any track.
    const auto placed = phazite::placeClip (context, b, beats (16), clip);
    REQUIRE (placed.isValid());
    CHECK (automates (placed) == second);
}

TEST_CASE ("A copied track's automation clips set the copy's devices, and others what they set")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);
    phazite::addEffect (context, a, song.find ("phaziteCompressor"));
    const ParameterRef second { phazite::getEffects (context, a)[1].id, "threshold" };
    const auto master = phazite::getAutomationTargets (context, phazite::getMasterTrack (context))[0].parameter;
    phazite::createAutomationClip (context, second, { beats (0), beats (4) });
    const auto masterClip = phazite::createAutomationClip (context, master, { beats (4), beats (8) });
    phazite::moveClip (context, masterClip, beats (4), a);
    REQUIRE (phazite::getClips (context, a).size() == 2);

    const auto copies = phazite::duplicateTracks ({ song.a }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto copy = idOf (*copies[0]);
    const auto copiedEffects = phazite::getEffects (context, copy);
    REQUIRE (copiedEffects.size() == 2);

    const auto clips = phazite::getClips (context, copy);
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].automates == ParameterRef { copiedEffects[1].id, "threshold" });
    CHECK (clips[1].automates == master);
    CHECK (phazite::getClips (context, a)[0].automates == second);
}

TEST_CASE ("An automation clip holds no notes: the piano roll doesn't show it, and its track needs no instrument for it")
{
    Song song;
    auto& context = song.context;
    const auto b = idOf (*song.b); // without an instrument
    const auto clip = phazite::createAutomationClip (context, phazite::getVolume (context, b).getRef(), { beats (0), beats (4) });
    REQUIRE (phazite::getClip (context, clip)->track == b);

    CHECK (phazite::getNoteClips (context, { clip }).empty());
    CHECK_FALSE (phazite::getNoteClip (context, clip).has_value());
    CHECK_FALSE (phazite::getMidiClipNearest (context, b, beats (2)).has_value());
    CHECK_FALSE (phazite::needsInstrument (*song.b));

    // A MIDI clip still does.
    const auto midi = phazite::addMidiClipAt (context, b, beats (8));
    CHECK (phazite::getMidiClipNearest (context, b, beats (2)) == midi);
    CHECK (phazite::needsInstrument (*song.b));
}

TEST_CASE ("A deleted automation clip sets nothing, and its parameter's value is the song's again")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    phazite::getParameter (context, threshold).setValue (-20.0f);
    const auto clip = phazite::createAutomationClip (context, threshold, { beats (0), beats (4) });
    phazite::replaceAutomationPoints (context, CurveRef (threshold, clip), { beats (0), beats (4) },
                                      { { beats (0), -50.0f }, { beats (4), -50.0f } });
    CHECK (song.valueAt (threshold, 2.0) == doctest::Approx (-50.0f));

    // What sets the parameter as the song plays, in its device's state.
    auto device = te::findPluginForID (*song.edit, toTracktion (threshold.device));
    REQUIRE (device != nullptr);
    CHECK (phazite::isAutomatedValue (device->state, "threshold"));

    {
        const phazite::UndoStep step (*song.edit, "Delete Clips");
        phazite::deleteClips (context, { clip }, false);
    }

    CHECK (song.valueAt (threshold, 2.0) == doctest::Approx (-20.0f));
    CHECK_FALSE (phazite::isAutomatedValue (device->state, "threshold"));

    phazite::undo (*song.edit);
    CHECK (song.valueAt (threshold, 2.0) == doctest::Approx (-50.0f));
    CHECK (phazite::isAutomatedValue (device->state, "threshold"));
}

TEST_CASE ("Undo while an automation clip sets a parameter leaves the parameter's own value as it was")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    phazite::getParameter (context, threshold).setValue (-20.0f);
    const auto clip = phazite::createAutomationClip (context, threshold, { beats (0), beats (4) });
    phazite::replaceAutomationPoints (context, CurveRef (threshold, clip), { beats (0), beats (4) },
                                      { { beats (0), -50.0f }, { beats (4), -50.0f } });
    const auto parameter = phazite::findParameter (*song.edit, threshold);

    // The clip sets it where the playhead is, and Tracktion stores that.
    CHECK (song.valueAt (threshold, 2.0) == doctest::Approx (-50.0f));
    phazite::test::dispatchPendingMessages();

    // Something else undone: the parameter's own value is still the one set by hand.
    phazite::addMidiClipAt (context, idOf (*song.b), beats (8));
    phazite::undo (*song.edit);
    CHECK (parameter->getCurrentExplicitValue() == doctest::Approx (-20.0f));
    CHECK (song.valueAt (threshold, 6.0) == doctest::Approx (-20.0f));

    // A change by hand undone plays the value before it.
    phazite::getParameter (context, threshold).setValue (-10.0f);
    CHECK (song.valueAt (threshold, 6.0) == doctest::Approx (-10.0f));
    phazite::undo (*song.edit);
    CHECK (parameter->getCurrentValue() == doctest::Approx (-20.0f));
    CHECK (song.valueAt (threshold, 2.0) == doctest::Approx (-50.0f));
}

TEST_CASE ("An automation clip whose device is gone stays one, setting nothing")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    const auto clip = phazite::createAutomationClip (context, threshold, { beats (0), beats (4) });

    phazite::removeDevice (context, threshold.device);
    auto info = phazite::getClip (context, clip);
    REQUIRE (info.has_value());
    CHECK (info->kind == phazite::ClipType::automation);
    CHECK_FALSE (info->automates.isValid());
    CHECK (phazite::getAutomationPoints (context, CurveRef (threshold, clip)).empty());

    phazite::undo (*song.edit);
    info = phazite::getClip (context, clip);
    CHECK (info->automates == song.threshold());
    CHECK (phazite::getAutomationPoints (context, CurveRef (song.threshold(), clip)).size() == 2);
}

TEST_CASE ("Playing an automation clip is no unsaved change, and a save in its middle keeps the value set by hand")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Clips", error);
    REQUIRE (project != nullptr);
    phazite::test::settle();

    const auto file = project->getFile();
    auto& edit = project->getEdit();
    auto* track = te::getAudioTracks (edit)[0];
    auto* volume = track->getVolumePlugin();
    REQUIRE (volume != nullptr);
    auto& parameter = *volume->volParam;

    // Set by hand, then an automation clip that sets it lower from beat 4 to 8.
    const auto setByHand = 0.5f, automated = 0.2f;
    parameter.setParameter (setByHand, juce::sendNotification);
    REQUIRE (phazite::createAutomationClip (*track, parameter, { te::BeatPosition::fromBeats (4.0), te::BeatPosition::fromBeats (8.0) },
                                            automated)
             != nullptr);
    phazite::test::settle(); // the loop follows the song's new end
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // With the playhead in it: what Tracktion writes into the device's state as the clip plays is no change.
    const auto inClip = edit.tempoSequence.toTime (te::BeatPosition::fromBeats (6.0));
    edit.getTransport().setPosition (inClip);
    parameter.updateToFollowCurve (inClip);
    phazite::test::settle();
    CHECK (parameter.getCurrentValue() == doctest::Approx (automated));
    CHECK (volume->state[te::IDs::volume].operator float() == doctest::Approx (automated));
    CHECK_FALSE (project->hasUnsavedChanges());

    // Saved there and opened again, the parameter has the value it was set to by hand, which it has outside the clip.
    REQUIRE (project->save().wasOk());
    project = nullptr;
    project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (project != nullptr);
    auto* reopened = te::getAudioTracks (project->getEdit())[0]->getVolumePlugin();
    REQUIRE (reopened != nullptr);
    CHECK (reopened->volParam->getCurrentExplicitValue() == doctest::Approx (setByHand));
    reopened->volParam->updateToFollowCurve (te::TimePosition());
    CHECK (reopened->volParam->getCurrentValue() == doctest::Approx (setByHand));
    reopened->volParam->updateToFollowCurve (inClip);
    CHECK (reopened->volParam->getCurrentValue() == doctest::Approx (automated));
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());

    // A change by hand is one.
    reopened->volParam->setParameter (0.7f, juce::sendNotification);
    phazite::test::settle();
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("An automation clip drives its parameter as the song plays")
{
    Song song;
    auto& context = song.context;
    const auto threshold = song.threshold();
    const auto clip = phazite::createAutomationClip (context, threshold, { beats (0), beats (16) });
    phazite::replaceAutomationPoints (context, CurveRef (threshold, clip), { beats (0), beats (16) },
                                      { { beats (0), -45.0f }, { beats (16), -45.0f } });
    const auto parameter = phazite::findParameter (*song.edit, threshold);
    parameter->updateToFollowCurve (te::TimePosition::fromSeconds (20.0)); // after the clip: its own value
    const auto ownValue = parameter->getCurrentValue();
    REQUIRE (std::abs (ownValue - -45.0f) > 1.0f);

    const phazite::test::RealTimePlayback playback (phazite::test::getEngine());
    auto& transport = song.edit->getTransport();
    transport.setPosition (song.edit->tempoSequence.toTime (te::BeatPosition::fromBeats (2.0)));
    transport.play (false);
    phazite::test::RealTimePlayback::wait (400);
    CHECK (parameter->getCurrentValue() == doctest::Approx (-45.0f));
    transport.stop (false, false);
    phazite::test::RealTimePlayback::wait (200);
}

//==============================================================================
namespace
{
// A playlist in a window showing the song, with the commands and keys the app gives it, and an automation clip for A's
// compressor's threshold from beat 4 to 12, with points at its ends.
struct ClipScreen
{
    ClipScreen()
    {
        clip = phazite::createAutomationClip (song.context, song.threshold(), { beats (4), beats (12) });
        phazite::replaceAutomationPoints (song.context, getCurve(), { beats (4), beats (12) },
                                          { { beats (4), -60.0f }, { beats (12), 0.0f } });
        song.edit->getUndoManager().clearUndoHistory();

        phazite::addSelectionCommands (registry, getSelection (song.context), getRangeSelection (song.context));
        phazite::ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        playlist.setGrid ({ .adaptive = false, .fixedLevel = 9 }); // a beat
        window.addKeyListener (&shortcutKeys);                     // as the main window does
        phazite::test::dispatchPendingMessages();
    }

    ~ClipScreen() { window.removeKeyListener (&shortcutKeys); }

    CurveRef getCurve() const { return { song.threshold(), clip }; }

    // The clip's curve, which the mouse uses.
    juce::Component& curve()
    {
        auto* found = phazite::test::findComponentWithTitle (playlist, "Automation clip of Compressor: Threshold");
        REQUIRE (found != nullptr);
        return *found;
    }

    // Where a beat and a threshold (from -60 to 0 dB) are on the clip's curve, which spans the clip.
    juce::Point<int> at (double beat, float decibels)
    {
        auto& view = curve();
        const auto& timeline = playlist.getView();
        const auto clipStart = phazite::getClip (song.context, clip)->range.getStart();
        const auto area = view.getLocalBounds().toFloat().reduced (0.0f, phazite::ui::LaneTint::pointRadius + 1.0f);
        return { juce::roundToInt (timeline.beatToX (beats (beat)) - std::round (timeline.beatToX (clipStart))),
                 juce::roundToInt (area.getBottom() - (decibels + 60.0f) / 60.0f * area.getHeight()) };
    }

    std::vector<phazite::AutomationPoint> getPoints() const { return phazite::getAutomationPoints (song.context, getCurve()); }
    std::vector<double> getBeats() const { return beatsOf (getPoints()); }

    Song song;
    ClipID clip;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry }; // with the default keymap
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::Playlist playlist { commands, song.context };
    phazite::test::TestWindow window { playlist, 1000, 600 };
};
} // namespace

TEST_CASE ("An automation clip shows its curve, whose points the Smart Tool drags, and a double-click adds one")
{
    ClipScreen screen;
    auto& curve = screen.curve();
    CHECK (curve.isShowing());
    CHECK (curve.getWidth() > 100);

    // The first point, dragged up and right on the grid, as one step.
    phazite::test::Mouse mouse (curve);
    mouse.press (screen.at (4.0, -60.0f));
    mouse.dragTo (screen.at (5.0, -45.0f));
    mouse.dragTo (screen.at (6.1, -30.0f));
    mouse.release();
    mouse.pause();
    CHECK (screen.getBeats() == std::vector { 6.0, 12.0 });
    CHECK (screen.getPoints()[0].value == doctest::Approx (-30.0f).epsilon (0.1));
    CHECK (screen.song.describeUndo() == juce::String ("Undo Move Automation Points"));

    // No further than the clip's start.
    mouse.press (screen.at (6.0, screen.getPoints()[0].value));
    mouse.dragTo (screen.at (1.0, -30.0f));
    mouse.release();
    mouse.pause();
    CHECK (screen.getBeats() == std::vector { 4.0, 12.0 });

    // A double-click where nothing is adds a point there; on a point, deletes it.
    mouse.doubleClick (screen.at (10.0, -55.0f));
    CHECK (screen.getBeats() == std::vector { 4.0, 10.0, 12.0 });
    CHECK (screen.getPoints()[1].value == doctest::Approx (-55.0f).epsilon (0.1));
    CHECK (screen.song.describeUndo() == juce::String ("Undo Add Automation Point"));
    CHECK (screen.song.context.getSongSelection().getSelectedAutomationPoints().indices == std::vector { 1 });

    mouse.doubleClick (screen.at (10.0, screen.getPoints()[1].value));
    CHECK (screen.getBeats() == std::vector { 4.0, 12.0 });
}

TEST_CASE ("Elsewhere on an automation clip, the Smart Tool moves the clip with its curve")
{
    ClipScreen screen;

    // Low on the clip, away from its line, which rises from its bottom left to its top right; from the playlist, which
    // stays where it is while the clip moves.
    auto& playlist = screen.playlist;
    const auto start = playlist.getLocalPoint (&screen.curve(), screen.at (7.0, -55.0f));
    const auto beatsAcross = [&playlist] (double b) { return juce::roundToInt (b * playlist.getView().pixelsPerBeat); };
    phazite::test::Mouse mouse (playlist);
    mouse.press (start);
    mouse.dragTo (start + juce::Point<int> (beatsAcross (2.0), 0));
    mouse.dragTo (start + juce::Point<int> (beatsAcross (4.0), 0));
    mouse.release();
    mouse.pause();

    const auto info = phazite::getClip (screen.song.context, screen.clip);
    REQUIRE (info.has_value());
    CHECK (info->range.getStart().inBeats() == doctest::Approx (8.0));
    CHECK (screen.getBeats() == std::vector { 8.0, 16.0 });
    CHECK (screen.song.describeUndo() == juce::String ("Undo Move Clip"));
}

TEST_CASE ("Draw draws an automation clip's curve, within the clip")
{
    ClipScreen screen;
    screen.playlist.setTool (phazite::ui::Tool::draw);
    phazite::test::Mouse mouse (screen.curve());

    // A step for each beat the mouse passes.
    mouse.press (screen.at (5.5, -20.0f));
    mouse.dragTo (screen.at (6.5, -20.0f));
    mouse.dragTo (screen.at (7.5, -20.0f));
    mouse.release();
    mouse.pause();
    CHECK (screen.getBeats() == std::vector { 4.0, 5.0, 6.0, 6.0, 7.0, 7.0, 8.0, 12.0 });
    CHECK (screen.getPoints()[1].value == doctest::Approx (-20.0f).epsilon (0.1));
    CHECK (screen.song.describeUndo() == juce::String ("Undo Draw Automation"));

    // Past the clip's end, nothing goes after it.
    mouse.press (screen.at (10.5, -10.0f));
    mouse.dragTo (screen.at (13.5, -10.0f));
    mouse.dragTo (screen.at (15.5, -10.0f));
    mouse.release();
    mouse.pause();
    const auto drawn = screen.getBeats();
    CHECK (drawn.back() == doctest::Approx (12.0));
}

TEST_CASE ("An automation clip's curve leaves its edges to trimming, and other tools to the clip")
{
    ClipScreen screen;
    auto& curve = screen.curve();
    const auto middle = screen.at (8.0, -30.0f); // on its line
    CHECK (curve.hitTest (middle.x, middle.y));
    CHECK_FALSE (curve.hitTest (2, middle.y));                                        // its start's edge
    CHECK_FALSE (curve.hitTest (screen.at (8.0, -5.0f).x, screen.at (8.0, -5.0f).y)); // nothing there

    screen.playlist.setTool (phazite::ui::Tool::draw);
    CHECK (curve.hitTest (screen.at (8.0, -5.0f).x, screen.at (8.0, -5.0f).y));
    CHECK_FALSE (curve.hitTest (curve.getWidth() - 2, middle.y)); // its end's edge

    screen.playlist.setTool (phazite::ui::Tool::slice);
    CHECK_FALSE (curve.hitTest (middle.x, middle.y));

    // The edge trims the clip.
    screen.playlist.setTool (phazite::ui::Tool::smart);
    phazite::test::Mouse mouse (curve);
    mouse.press ({ curve.getWidth() - 2, middle.y });
    mouse.dragTo (screen.at (13.0, -30.0f));
    mouse.dragTo (screen.at (16.0, -30.0f));
    mouse.release();
    mouse.pause();
    CHECK (phazite::getClip (screen.song.context, screen.clip)->range.getEnd().inBeats() == doctest::Approx (16.0));
}
