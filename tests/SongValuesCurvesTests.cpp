// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's automation change hears it from one place (model/base/internal/SongValues.h:
// Listener::curvesChanged()), by the parameters of the curves that changed (roadmap step S5.20). For each edit of the curves
// made through the model, its Undo and its Redo, the listener is told exactly the parameters whose curves differ before and
// after, compared as values (independent of what tells it), and nothing of another area; another area's change tells it
// nothing; and what counts the song's changes hears each change once: the project's count of changes and the versions'
// count of the song's changes go up by what the listener was told, and by nothing else, where no other area told anything.
// And the versions give the song another song's curves by their parameters, through automation's functions
// (model/versions/ApplySong.cpp: makeCurvesLike(), in one set of the curves' area; Automation.h: setCurveValue(),
// eraseCurve()): the whole song's, each one exact undo step, in the version's order, and a line's, its devices' alone;
// a song made as it was copied writes nothing of its curves; and a merge's clash on a curve is chosen either way.
//   phazite_tests.exe --test-case="Song values' curves*"
#include "EnginePlayback.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UndoCheck.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/TracksValue.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace
{
using phazite::test::at;
using phazite::test::lasting;

// A parameter as text, for the checks' messages and to sort them.
juce::String describe (const phazite::ParameterRef& parameter)
{
    return phazite::getCurveKeyText (parameter);
}

juce::StringArray describe (const std::vector<phazite::ParameterRef>& parameters)
{
    juce::StringArray texts;

    for (const auto& parameter : parameters)
        texts.add (describe (parameter));

    texts.sort (false);
    return texts;
}

// How many calls the values' listener heard, of each area, and the parameters of the curves it was told of, each once.
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++clips; }
    void tempoMapChanged() override { ++tempo; }

    // The tracks', which a track removed, duplicated or moved comes and goes with since the tracks' switch.
    void tracksChanged (const std::vector<phazite::TrackChange>&, const std::vector<phazite::TrackID>&, const phazite::SongTracks&) override
    {
        ++tracksTold;
    }

    // The devices', which a device removed comes and goes with since the devices' switch.
    void devicesChanged (const std::vector<phazite::SongValues::DeviceChange>&, const std::vector<phazite::DevicePlace>&,
                         const phazite::SongDevices&) override
    {
        ++devices;
    }

    void curvesChanged (const std::vector<phazite::ParameterRef>& told) override
    {
        ++curves;
        CHECK_FALSE (told.empty());

        for (size_t i = 0; i < told.size(); ++i)
        {
            CHECK (std::find (told.begin(), told.begin() + (std::ptrdiff_t) i, told[i]) == told.begin() + (std::ptrdiff_t) i); // once
            if (std::find (parameters.begin(), parameters.end(), told[i]) == parameters.end())
                parameters.push_back (told[i]);
        }
    }

    int notes = 0, clips = 0, tempo = 0, curves = 0, tracksTold = 0, devices = 0;
    std::vector<phazite::ParameterRef> parameters;
};

// How often the song's curves' area moved to another value (SongValues::curves), heard as the area itself tells its
// listeners, without asking the song.
struct AreaMoves final : phazite::ValueArea<phazite::SongCurves>::Listener
{
    explicit AreaMoves (phazite::ValueArea<phazite::SongCurves>& areaToHear)
        : area (areaToHear)
    {
        area.addListener (this);
    }

    ~AreaMoves() override { area.removeListener (this); }

    void valueAreaChanged (const phazite::SongCurves& before, const phazite::SongCurves& after) override
    {
        moves += before.isSameAs (after) ? 0 : 1;
    }

    phazite::ValueArea<phazite::SongCurves>& area;
    int moves = 0;
};

// The parameters whose curves differ between two values of the song's curves: one in one and not the other, or with other
// points; and of those alike in both, those that came to other places among them (the curves' order, which a file holds).
// Independent of what the listener is told.
std::vector<phazite::ParameterRef> findDiffering (const phazite::SongCurves& before, const phazite::SongCurves& after)
{
    std::vector<phazite::ParameterRef> differing;

    for (const auto* curves : { &before, &after })
    {
        for (const auto& parameter : curves->order)
        {
            const auto* was = before.find (parameter);
            const auto* is = after.find (parameter);

            if ((was == nullptr || is == nullptr || ! (*was == *is))
                && std::find (differing.begin(), differing.end(), parameter) == differing.end())
                differing.push_back (parameter);
        }
    }

    const auto alike = [&differing] (const phazite::SongCurves& curves)
    {
        std::vector<phazite::ParameterRef> kept;

        for (const auto& parameter : curves.order)
            if (std::find (differing.begin(), differing.end(), parameter) == differing.end())
                kept.push_back (parameter);

        return kept;
    };

    const auto was = alike (before), is = alike (after);

    for (size_t i = 0; i < was.size() && i < is.size(); ++i)
        if (was[i] != is[i])
            differing.push_back (is[i]);

    return differing;
}
// A song in a project of its own, with its versions and the values' listener recording what they are told. It holds a
// curve from the start.
// are told. It holds a curve from the start, so that its AUTOMATION is there before the edits.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        phazite::addAutomationPoint (s.context, s.getPanParameter (s.firstTrack), { at (0.0), 0.5f });
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    // What a change told: the values' listener; how far the area moved; how far the project's count of changes and the
    // versions' count of the song's changes went up meanwhile; the parameters whose curves differ after it; and the song's
    // curves before it and after.
    struct Heard
    {
        Told told;
        int moves = 0;
        juce::int64 project = 0, versions = 0;
        std::vector<phazite::ParameterRef> differing;
        phazite::SongCurves before, after;
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        area.moves = 0;
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        const auto before = phazite::getCurvesValue (song);
        change();
        const auto after = phazite::getCurvesValue (song);
        return { told,
                 area.moves,
                 s.getProject().getChangeCount() - projectBefore,
                 versions->countSongChanges() - versionsBefore,
                 findDiffering (before, after),
                 before,
                 after };
    }

    // A change as one undo step, then its Undo, then its Redo: what each told.
    std::vector<Heard> hearEach (const juce::String& name, const std::function<void()>& change)
    {
        return { hear (
                     [&]
                     {
                         const phazite::UndoStep step (song, name);
                         change();
                     }),
                 hear ([this] { phazite::undo (song); }), hear ([this] { phazite::redo (song); }) };
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told;
    AreaMoves area { song.getValues()->curves };
};

// What a change and its Undo and Redo told, each against the parameters whose curves differ: exactly those, a parameter told
// whose curve is there and the same before and after, or that names no curve before nor after (a curve given another
// parameter in two writes, its device and then its parameter, passing through one), failing it; once for each change of
// the curves' value; nothing of another area unless othersToo; and, where no other area told, the project's and the
// versions' counts up by what the listener was told.
void checkHeard (const juce::String& name, const std::vector<Watched::Heard>& heard, bool othersToo = false)
{
    INFO (name.toStdString());
    const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
    REQUIRE (heard.size() <= moments.size());

    for (size_t i = 0; i < heard.size(); ++i)
    {
        INFO (moments[i]);
        const auto& each = heard[i];
        const auto& told = each.told.parameters;
        CHECK_MESSAGE (describe (told) == describe (each.differing), "told " << describe (told).joinIntoString (", ") << "; differing "
                                                                             << describe (each.differing).joinIntoString (", "));
        CHECK (each.told.curves == each.moves);

        if (! othersToo)
        {
            CHECK (each.told.clips == 0);
            CHECK (each.told.notes == 0);
            CHECK (each.told.tempo == 0);
        }

        // Heard once: what counts the song's changes counts what it was told (where nothing else changed: the tracks, or
        // the devices, which a device removed comes and goes with).
        if (each.told.devices == 0 && each.told.tracksTold == 0)
        {
            CHECK (each.project == each.told.curves);
            CHECK (each.versions == each.told.curves);
        }
    }
}

std::vector<int> allIndices (const phazite::Song& song, const phazite::ParameterRef& parameter)
{
    std::vector<int> indices ((size_t) phazite::getPoints (song, parameter).size());

    for (size_t i = 0; i < indices.size(); ++i)
        indices[i] = (int) i;

    return indices;
}

// The first parameter of a device that can be automated, on its track.
phazite::ParameterRef findDeviceParameter (const phazite::Song& song, phazite::TrackID track, phazite::DeviceID device)
{
    for (const auto& parameter : phazite::getAutomatableParameters (song, track))
        if (parameter.device == device)
            return parameter;

    return {};
}

// Points given to a parameter's curve, in a step of their own.
void putPoints (phazite::Song& song, const phazite::ParameterRef& parameter, std::initializer_list<phazite::AutomationPoint> points)
{
    const phazite::UndoStep step (song, "Points");

    for (const auto& point : points)
        phazite::addPoint (song, parameter, point);
}

// A song's AUTOMATION as text (none for a song without one), from a whole copy of it or a version's song.
juce::String writeAutomation (const juce::ValueTree& song)
{
    const auto automation = song.getChildWithName (phazite::schema::AUTOMATION);
    return automation.isValid() ? automation.toXmlString() : juce::String ("none");
}

// A parameter's curve in a whole copy of a song or a version's song, as the song reads one; none for one without.
std::optional<phazite::CurveValue> findCurveIn (const juce::ValueTree& song, const phazite::ParameterRef& parameter)
{
    const auto curves = phazite::readCurvesValue (song.getChildWithName (phazite::schema::AUTOMATION));

    if (const auto* curve = curves.find (parameter))
        return *curve;

    return std::nullopt;
}
} // namespace

TEST_CASE ("Song values' curves: each edit of a curve through the model tells the parameters whose curves changed, its Undo and "
           "its Redo too, each once, and nothing of another area")
{
    Watched w;
    auto& song = w.song;
    const auto volume = w.s.getVolumeParameter (w.s.firstTrack);
    const auto pan = w.s.getPanParameter (w.s.firstTrack);

    checkHeard ("Add Automation Point", w.hearEach ("Add Automation Point", [&] { phazite::addPoint (song, volume, { at (0.0), 0.5f }); }));
    checkHeard ("Add Automation Points", w.hearEach ("Add Automation Points",
                                                     [&]
                                                     {
                                                         phazite::addPoint (song, volume, { at (4.0), 0.25f });
                                                         phazite::addPoint (song, volume, { at (8.0), 0.75f });
                                                         phazite::addPoint (song, pan, { at (8.0), 0.2f });
                                                     }));
    REQUIRE (phazite::getPoints (song, volume).size() == 3);

    checkHeard ("Move Automation Point",
                w.hearEach ("Move Automation Point", [&] { phazite::movePoints (song, volume, { 1 }, lasting (1.0), 0.1f); }));
    checkHeard ("Bend Automation", w.hearEach ("Bend Automation", [&] { phazite::setCurve (song, volume, 0, 0.5f); }));
    checkHeard ("Move Automation Points",
                w.hearEach ("Move Automation Points", [&] { phazite::movePoints (song, volume, { 0, 1, 2 }, lasting (0.5), -0.05f); }));
    checkHeard (
        "Draw Automation",
        w.hearEach ("Draw Automation",
                    [&] { phazite::replacePoints (song, volume, { at (2.0), at (6.0) }, { { at (2.5), 0.2f }, { at (5.0), 0.9f } }); }));
    checkHeard ("Delete Automation Point", w.hearEach ("Delete Automation Point", [&] { phazite::deletePoints (song, volume, { 0 }); }));
    checkHeard ("Insert Time", w.hearEach ("Insert Time", [&] { phazite::insertTime (song, at (3.0), lasting (4.0)); }), true);
    checkHeard ("Remove Time", w.hearEach ("Remove Time", [&] { phazite::removeTime (song, { at (1.0), at (4.0) }); }), true);

    // A curve cleared: its points all taken out, and the curve with them.
    checkHeard ("Clear Automation",
                w.hearEach ("Clear Automation", [&] { phazite::deletePoints (song, volume, allIndices (song, volume)); }));
    CHECK_FALSE (phazite::getCurveValue (song, volume).has_value());

    // A point set where the curve already is: no change, nothing told.
    const auto same = w.hear ([&] { phazite::setCurve (song, pan, 0, phazite::getPoints (song, pan).front().curve); });
    CHECK (same.told.curves == 0);
    CHECK (same.project == 0);
}

TEST_CASE ("Song values' curves: what takes the curves along tells them: a device removed, a track duplicated, a folder made a bus "
           "and back, a version applied")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto eq = w.s.addEffect (track, phazite::devices::Equalizer::typeID);
    const auto eqParameter = findDeviceParameter (song, track, eq);
    REQUIRE (eqParameter.isValid());
    phazite::addAutomationPoint (w.s.context, eqParameter, { at (0.0), 0.3f });
    phazite::addAutomationPoint (w.s.context, eqParameter, { at (4.0), 0.7f });
    phazite::addAutomationPoint (w.s.context, w.s.getVolumeParameter (track), { at (2.0), 0.4f });

    // A device removed takes its curves along, in its step (Mixer.h: forgetWhatNamesTheGone()): told once, its Undo and its
    // Redo too, its curve alone.
    const auto removed = w.hearEach ("Remove Device", [&] { phazite::removeDevice (song, eq); });
    checkHeard ("Remove Device", removed);

    for (const auto& each : removed)
    {
        CHECK (each.told.curves == 1);
        CHECK (describe (each.differing) == describe (std::vector { eqParameter }));
    }

    phazite::undo (song); // the EQ back
    REQUIRE (phazite::test::hasDevice (song, eq));

    // A track duplicated with its fader's and its devices' curves: the copies' curves, under their new IDs.
    const auto duplicated = w.hearEach ("Duplicate Track", [&] { phazite::duplicateTracks (song, { track }, nullptr); });
    checkHeard ("Duplicate Track", duplicated, true);
    CHECK (duplicated.front().differing.size() == 3); // the copy's pan, volume and EQ's

    // A folder's volume curve given to the fader that takes its place, each way: the parameter it had and the one it has, at
    // once, in one call (no parameter in between), its Undo and its Redo too; in its place among the song's curves.
    const auto bus = w.s.addBus ("Bus", { track });
    phazite::addAutomationPoint (w.s.context, w.s.getVolumeParameter (bus), { at (1.0), 0.6f });
    const auto toPlain = w.hearEach ("Make Plain Folder", [&] { phazite::makePlainFolder (song, bus); });
    checkHeard ("Make Plain Folder", toPlain, true);
    CHECK (toPlain.front().differing.size() == 2);

    for (const auto& each : toPlain)
    {
        CHECK (each.told.curves == 1);
        CHECK (each.before.order.size() == each.after.order.size());
    }

    const auto toBus = w.hearEach ("Make Bus", [&] { phazite::makeBus (song, bus); });
    checkHeard ("Make Bus", toBus, true);

    for (const auto& each : toBus)
        CHECK (each.told.curves == 1);

    // A version with other curves applied, each way, in a step and outside the history.
    const auto before = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Other Curves");
        phazite::addPoint (song, w.s.getPanParameter (track), { at (6.0), 0.9f });
        phazite::deletePoints (song, w.s.getVolumeParameter (track), allIndices (song, w.s.getVolumeParameter (track)));
        phazite::addPoint (song, w.s.getVolumeParameter (bus), { at (3.0), 0.1f });
    }

    const auto after = phazite::test::copyWholeSong (song);
    const auto apply = [&song] (const juce::ValueTree& version)
    {
        phazite::makeSongLike (song, version);
        phazite::removeBrokenReferences (song);
    };

    const auto back = w.hearEach ("Bring Back", [&] { apply (before); });
    checkHeard ("Bring Back", back, true);
    CHECK (back.front().differing.size() == 3);
    checkHeard ("Bring Back Again", w.hearEach ("Bring Back Again", [&] { apply (after); }), true);
    checkHeard ("Another Branch", { w.hear ([&] { phazite::makeSongTheSame (song, before); }) }, true);
    checkHeard ("Back on the Branch", { w.hear ([&] { phazite::makeSongTheSame (song, after); }) }, true);
}

TEST_CASE ("Song values' curves: automation written while the song plays tells the curve it wrote, its Undo and its Redo too")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    w.s.setTempo (120.0);
    w.s.addMidiClip (track, 0.0, 16.0);
    const auto lane = w.s.getVolumeParameter (track);
    w.s.addAutomation (lane, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (16.0), 0.5f } });
    phazite::setAutomationMode (w.s.context, track, phazite::AutomationMode::touch);
    phazite::test::EnginePlayback playback;
    auto control = phazite::getVolume (w.s.context, track);

    // One pass of Touch, written as the control is let go: its curve alone.
    std::vector<Watched::Heard> heard;
    heard.push_back (w.hear (
        [&]
        {
            w.s.run ("transport.playStop");
            playback.play (0.5);
            control.beginGesture();
            control.setValue (0.3f);
            playback.play (0.5);
            control.setValue (0.6f);
            playback.play (0.5);
            control.endGesture();
            playback.play (0.25);
            w.s.run ("transport.playStop");
        }));

    REQUIRE (heard.front().differing.size() == 1);
    CHECK (heard.front().differing.front() == lane);
    heard.push_back (w.hear ([&song] { phazite::undo (song); }));
    heard.push_back (w.hear ([&song] { phazite::redo (song); }));
    checkHeard ("Record Automation", heard, true);
}

TEST_CASE ("Song values' curves: another area's change tells nothing of the curves, nor its Undo and its Redo")
{
    Watched w;
    auto& song = w.song;
    const auto track = w.s.firstTrack;
    const auto clip = w.s.addMidiClip (track, 0.0, 4.0);
    REQUIRE (clip.isValid());

    const auto check = [&w] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        for (const auto& heard : w.hearEach (name, change))
        {
            CHECK (heard.told.curves == 0);
            CHECK (heard.differing.empty());
        }
    };

    check ("Add Clip", [&] { CHECK (phazite::insertMidiClip (song, track, { at (8.0), at (12.0) }, nullptr).isValid()); });
    check ("Add Note", [&] { CHECK (phazite::test::addMidiNote (song, clip, 60, at (1.0), lasting (1.0)).isValid()); });
    check ("Mute Clip", [&] { phazite::toggleClipsMuted (song, { clip }); });
    check ("Rename Track", [&] { phazite::setTrackName (song, track, "Renamed"); });
    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); });
    check ("Add Effect", [&] { w.s.addEffect (track, phazite::devices::Equalizer::typeID); });
    check ("Add Track", [&] { CHECK (phazite::insertTrack (song, track, nullptr).isValid()); });
}

TEST_CASE ("Song values' curves: a version with other curves applied both ways is one exact undo step each, its curves the "
           "version's, in its order")
{
    // A curve added since, one taken out, one's points changed and made again (last in the order since), one of a device the
    // version has and the song hasn't: each made the version's by the curves pass, which leaves the passes over elements
    // nothing of the curves (ApplySong.cpp: makeCurvesLike()); and a version without automation, whose curves the pass
    // takes away (the song's AUTOMATION, the empty place of its curves, staying; a whole copy of it writes none).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto eq = s.addEffect (track, phazite::devices::Equalizer::typeID);
    const auto eqParameter = findDeviceParameter (song, track, eq);
    REQUIRE (eqParameter.isValid());
    const auto volume = s.getVolumeParameter (track), pan = s.getPanParameter (track), otherVolume = s.getVolumeParameter (other);
    putPoints (song, volume, { { at (0.0), 0.5f }, { at (4.0), 0.8f, 0.3f } });
    putPoints (song, eqParameter, { { at (0.0), 0.3f }, { at (8.0), 0.6f } });
    putPoints (song, otherVolume, { { at (2.0), 0.4f } });
    const auto before = phazite::test::copyWholeSong (song);
    auto without = before.createCopy();
    without.removeChild (without.getChildWithName (phazite::schema::AUTOMATION), nullptr);

    {
        const phazite::UndoStep step (song, "Other Curves");
        phazite::deletePoints (song, volume, allIndices (song, volume));
        phazite::addPoint (song, volume, { at (1.0), 0.25f });
        phazite::addPoint (song, volume, { at (6.0), 0.75f });
        phazite::addPoint (song, pan, { at (2.0), 0.6f });
        phazite::deletePoints (song, otherVolume, allIndices (song, otherVolume));
        phazite::removeDevice (song, eq); // its curve with it
    }

    const auto after = phazite::test::copyWholeSong (song);
    REQUIRE (phazite::getCurveKeys (song) == std::vector { volume, pan });
    phazite::test::UndoCheck undo (song);

    const auto apply = [&] (const juce::String& name, const juce::ValueTree& version)
    {
        INFO (name.toStdString());
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        phazite::makeSongLike (song, version);
                        phazite::removeBrokenReferences (song);
                    });
        CHECK (writeAutomation (phazite::test::copyWholeSong (song)) == writeAutomation (version));
    };

    apply ("Bring Back", before);
    CHECK (phazite::getCurveKeys (song) == std::vector { volume, eqParameter, otherVolume });
    CHECK (phazite::test::hasDevice (song, eq));
    apply ("Bring Back Again", after);
    CHECK (phazite::getCurveKeys (song) == std::vector { volume, pan });
    CHECK_FALSE (phazite::test::hasDevice (song, eq));
    apply ("Without Automation", without);
    CHECK (phazite::getCurveKeys (song).empty());
    CHECK_FALSE (phazite::test::copyWholeSong (song).getChildWithName (phazite::schema::AUTOMATION).isValid());
    apply ("Automation Again", before);
    apply ("The Same", before); // nothing differs: no step
    CHECK (undo.getNumSteps() == 4);
    undo.walkBackAndForth();

    // Outside the history, as a switch to another branch makes the song another's (makeSongTheSame()).
    for (const auto& version : { after, without, before })
    {
        phazite::makeSongTheSame (song, version);
        CHECK (writeAutomation (phazite::test::copyWholeSong (song)) == writeAutomation (version));
    }
}

TEST_CASE ("Song values' curves: a version without automation applied takes the song's curves away in one action, "
           "what hears the curves told them at once")
{
    // ApplySong.cpp: makeCurvesLike(), in one set of the curves' area. Three curves, and the song's sections beside them: the
    // step is one action, not one for each curve; what hears the curves change is told the three at once; a whole copy of
    // the song, without curves, writes no AUTOMATION, as the version has none, and its other elements as the version's.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto volume = s.getVolumeParameter (track), pan = s.getPanParameter (track), otherVolume = s.getVolumeParameter (other);
    putPoints (song, volume, { { at (0.0), 0.5f }, { at (4.0), 0.8f } });
    putPoints (song, pan, { { at (2.0), 0.3f } });
    putPoints (song, otherVolume, { { at (1.0), 0.4f } });

    {
        const phazite::UndoStep step (song, "Add Section");
        phazite::addSectionAt (song, at (0.0), nullptr);
    }

    auto version = phazite::test::copyWholeSong (song);
    version.removeChild (version.getChildWithName (phazite::schema::AUTOMATION), nullptr);

    const auto typesOf = [] (const juce::ValueTree& root)
    {
        juce::StringArray types;

        for (const auto& child : root)
            types.add (child.getType().toString());

        return types;
    };

    phazite::test::UndoCheck undo (song);
    Told told;
    song.getValues()->addListener (&told);
    auto actions = 0;
    undo.check ("Without Automation",
                [&]
                {
                    const phazite::UndoStep step (song, "Without Automation");
                    phazite::makeSongLike (song, version);
                    actions = song.getUndoManager().getNumActionsInCurrentTransaction();
                });
    song.getValues()->removeListener (&told);

    CHECK (actions == 1);
    CHECK (typesOf (phazite::test::copyWholeSong (song)) == typesOf (version));
    CHECK (describe (told.parameters) == describe (std::vector { volume, pan, otherVolume }));
    CHECK (told.curves == 3); // the step, its Undo, its Redo: each the three at once
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' curves: a track's line made a version's, and put back, takes its devices' curves and no other track's")
{
    // makeTrackLike() (ApplySong.cpp): the curves of what either song has on the track made the version's, by their
    // parameters, a curve the song hasn't added after the others; another track's left as they are.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto eq = s.addEffect (track, phazite::devices::Equalizer::typeID);
    const auto eqParameter = findDeviceParameter (song, track, eq);
    REQUIRE (eqParameter.isValid());
    const auto volume = s.getVolumeParameter (track), pan = s.getPanParameter (track), otherVolume = s.getVolumeParameter (other);
    putPoints (song, volume, { { at (0.0), 0.5f }, { at (4.0), 0.8f } });
    putPoints (song, eqParameter, { { at (0.0), 0.3f } });
    putPoints (song, otherVolume, { { at (2.0), 0.4f } });
    const auto before = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Other Curves");
        phazite::movePoints (song, volume, { 1 }, lasting (1.0), -0.2f);
        phazite::deletePoints (song, eqParameter, { 0 });
        phazite::addPoint (song, pan, { at (2.0), 0.6f });
        phazite::movePoints (song, otherVolume, { 0 }, lasting (2.0), 0.1f);
    }

    const auto otherCurve = phazite::getCurveValue (song, otherVolume);
    REQUIRE (otherCurve.has_value());
    phazite::test::UndoCheck undo (song);

    const auto isTheVersions = [&]
    {
        for (const auto& parameter : { volume, eqParameter, pan })
        {
            INFO (phazite::getCurveKeyText (parameter).toStdString());
            CHECK (phazite::getCurveValue (song, parameter) == findCurveIn (before, parameter));
        }

        CHECK (phazite::getCurveValue (song, otherVolume) == otherCurve);
    };

    const auto restore = [&] (const juce::String& name)
    {
        undo.check (name,
                    [&]
                    {
                        const phazite::UndoStep step (song, name);
                        CHECK (phazite::makeTrackLike (song, track, before));
                        phazite::removeBrokenReferences (song);
                    });
    };

    restore ("Restore the Track");
    isTheVersions();
    CHECK (phazite::getCurveKeys (song) == std::vector { volume, otherVolume, eqParameter });

    // Deleted, its curves with it, and a point given since to its volume's parameter, by the parameter alone, which makes that
    // curve again; put back from the version.
    undo.check ("Delete the Track",
                [&]
                {
                    const phazite::UndoStep step (song, "Delete the Track");
                    phazite::deleteTracks (song, { track }, nullptr);
                    phazite::addPoint (song, volume, { at (12.0), 0.1f });
                });
    REQUIRE_FALSE (phazite::test::hasTrack (song, track));
    restore ("Put Back the Track");
    REQUIRE (phazite::test::hasTrack (song, track));
    isTheVersions();

    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' curves: the song made as it was copied after an automation edit undone writes nothing of its curves")
{
    // makeSongAsCopied() (a version open put back): every change of a curve is an undo step, so the curves are the copy's
    // already, and the curves pass writes nothing (asserted in Debug builds as it ends).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto volume = s.getVolumeParameter (s.firstTrack), pan = s.getPanParameter (s.firstTrack);
    putPoints (song, volume, { { at (0.0), 0.5f }, { at (4.0), 0.8f } });
    putPoints (song, pan, { { at (2.0), 0.3f } });
    const auto copy = phazite::test::copyWholeSong (song);
    const auto copyState = phazite::getSongState (song);

    {
        const phazite::UndoStep step (song, "Edit");
        phazite::movePoints (song, volume, { 1 }, lasting (1.0), -0.2f);
        phazite::deletePoints (song, pan, { 0 });
    }

    phazite::undo (song);
    AreaMoves area { song.getValues()->curves };
    phazite::makeSongAsCopied (song, copyState);
    CHECK (area.moves == 0);
    CHECK (writeAutomation (phazite::test::copyWholeSong (song)) == writeAutomation (copy));
}

TEST_CASE ("Song values' curves: a merge's clash on a curve is chosen each way through automation's functions by its parameter")
{
    // A curve merges whole: both sides changed it, so the merge reports one clash on the whole CURVE, whose choice the
    // curves' chooser makes (Automation.cpp: chooseCurveClash()), in the choice's step, its points by their places.
    const auto base = phazite::createSong();
    const auto track = phazite::ensureTracks (*base, 1).front();
    const phazite::ParameterRef volume { phazite::getFader (*base, track), phazite::getVolumeParameterID (*base, track) };
    REQUIRE (volume.isValid());
    putPoints (*base, volume, { { at (0.0), 0.5f }, { at (4.0), 0.5f } });

    const auto ours = phazite::makeSong (phazite::test::copyWholeSong (*base));
    const auto theirs = phazite::makeSong (phazite::test::copyWholeSong (*base));
    REQUIRE (ours != nullptr);
    REQUIRE (theirs != nullptr);

    {
        const phazite::UndoStep step (*ours, "Move Point");
        phazite::movePoints (*ours, volume, { 1 }, lasting (0.0), 0.2f);
    }

    {
        const phazite::UndoStep step (*theirs, "Add Point");
        phazite::addPoint (*theirs, volume, { at (8.0), 0.1f });
    }

    const std::optional<phazite::CurveValue> curves[] = { phazite::getCurveValue (*base, volume), phazite::getCurveValue (*ours, volume),
                                                          phazite::getCurveValue (*theirs, volume) };
    const phazite::MergeSides sides { phazite::test::copyWholeSong (*base), phazite::test::copyWholeSong (*ours),
                                      phazite::test::copyWholeSong (*theirs) };
    const auto merged = phazite::mergeSongs (sides.base, sides.before, sides.theirs, [&ours] { return ours->makeID(); });
    const phazite::MergeClash* clash = nullptr;

    for (const auto& each : merged.clashes)
        if (each.path.size() >= 2 && each.path[0].startsWith (phazite::schema::AUTOMATION.toString() + ":"))
            clash = &each;

    REQUIRE (clash != nullptr);
    CHECK (clash->path.size() == 2);

    {
        const phazite::UndoStep step (*ours, "Merge");
        phazite::makeSongLike (*ours, merged.song);
    }

    phazite::test::UndoCheck undo (*ours);

    for (const auto value : { 2, 0, 1 })
    {
        INFO ("value " << value);
        undo.check ("Choose",
                    [&]
                    {
                        const phazite::UndoStep step (*ours, "Choose");
                        CHECK (phazite::chooseCurveClash (*ours, *clash, value, sides) == phazite::AreaChoice::chosen);
                    });
        CHECK (phazite::getCurveValue (*ours, volume) == curves[value]);
        phazite::test::checkSong (*ours);
    }

    // A clash elsewhere is none of the curves'; one of a curve's property, or of something in it, which a merge never makes,
    // refused.
    auto elsewhere = *clash;
    elsewhere.path = { phazite::schema::CHORDLANE.toString() + ":" + phazite::schema::CHORDLANE.toString() };
    CHECK (phazite::chooseCurveClash (*ours, elsewhere, 0, sides) == phazite::AreaChoice::notTheArea);
    auto ofProperty = *clash;
    ofProperty.property = phazite::schema::curveParameter;
    CHECK (phazite::chooseCurveClash (*ours, ofProperty, 0, sides) == phazite::AreaChoice::refused);
    auto inside = *clash;
    inside.path.add (phazite::schema::POINT.toString() + ":");
    CHECK (phazite::chooseCurveClash (*ours, inside, 0, sides) == phazite::AreaChoice::refused);

    // The side chosen without the curve: none.
    auto none = *clash;
    none.theirsElement = {};
    undo.check ("Choose None",
                [&]
                {
                    const phazite::UndoStep step (*ours, "Choose None");
                    CHECK (phazite::chooseCurveClash (*ours, none, 2, sides) == phazite::AreaChoice::chosen);
                });
    CHECK_FALSE (phazite::getCurveValue (*ours, volume).has_value());
    undo.walkBackAndForth();
    phazite::test::checkSong (*ours);
}
