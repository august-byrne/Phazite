// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's automation change hears it from one place (model/base/internal/SongValues.h:
// Listener::curvesChanged()), by the parameters of the curves that changed (roadmap step S5.20). For each edit of the curves
// made through the model, its Undo and its Redo, the listener is told exactly the parameters whose curves differ before and
// after, compared as values (independent of what tells it), and nothing of another area; another area's change tells it
// nothing; and what listens to the song's root hears each change once: the project's count of changes and the versions'
// count of the song's changes go up by what the listener was told, and by nothing else, where the tree told nothing but the
// curves. And the versions give the song another song's curves by their parameters, through automation's functions
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

    int notes = 0, clips = 0, tempo = 0, curves = 0;
    std::vector<phazite::ParameterRef> parameters;
};

// What a listener of the song's root is told, sorted as the test sees it, without asking the song: under a curve (a CURVE
// of the song's AUTOMATION, or what is in one; a CURVE added to it or taken out), the song's AUTOMATION added or taken out
// with curves in it, and all else.
struct TreeHeard final : juce::ValueTree::Listener
{
    explicit TreeHeard (const juce::ValueTree& rootToHear)
        : root (rootToHear)
    {
        root.addListener (this);
    }

    ~TreeHeard() override { root.removeListener (this); }

    static bool isUnderCurve (juce::ValueTree element)
    {
        for (; element.isValid(); element = element.getParent())
            if (element.hasType (phazite::schema::CURVE) && element.getParent().hasType (phazite::schema::AUTOMATION))
                return true;

        return false;
    }

    void hear (bool underCurve) { ++(underCurve ? curves : other); }

    void hearChild (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        if (child.hasType (phazite::schema::AUTOMATION) && child.getChildWithName (phazite::schema::CURVE).isValid())
            ++automationWithCurves;

        hear ((child.hasType (phazite::schema::CURVE) && parent.hasType (phazite::schema::AUTOMATION)) || isUnderCurve (parent));
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { hear (isUnderCurve (tree)); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { hearChild (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { hearChild (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { hear (isUnderCurve (parent)); }

    juce::ValueTree root;
    int curves = 0, automationWithCurves = 0, other = 0;
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

// A song in a project of its own, with its versions, the values' listener and a listener of its root recording what they
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

    // What a change told: the values' listener; the root's listener, under the curves, of an AUTOMATION added or taken out
    // with curves, and of everything else; how far the project's count of changes and the versions' count of the song's
    // changes went up meanwhile; the parameters whose curves differ after it; and the song's curves before it and after.
    struct Heard
    {
        Told told;
        int curves = 0, automationWithCurves = 0, other = 0, moves = 0;
        juce::int64 project = 0, versions = 0;
        std::vector<phazite::ParameterRef> differing;
        phazite::SongCurves before, after;
    };

    Heard hear (const std::function<void()>& change)
    {
        auto* versions = phazite::getVersionHistory (s.context);
        REQUIRE (versions != nullptr);
        told = {};
        tree.curves = tree.automationWithCurves = tree.other = 0;
        area.moves = 0;
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        const auto before = phazite::getCurvesValue (song);
        change();
        const auto after = phazite::getCurvesValue (song);
        return { told,
                 tree.curves,
                 tree.automationWithCurves,
                 tree.other,
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
    TreeHeard tree { song.getState() };
    AreaMoves area { song.getValues()->curves };
};

// How often the listener is told: since automation's switch (roadmap step S5.20) the area tells it once for each change of
// its value (a curve given another parameter included, in one change), and the song's tree tells nothing of the curves (a
// tree heard under them a failure, checked here).
int toldForTheTree (const Watched::Heard& heard)
{
    CHECK (heard.curves + heard.automationWithCurves == 0);
    return heard.moves;
}

// What a change and its Undo and Redo told, each against the parameters whose curves differ: exactly those, a parameter told
// whose curve is there and the same before and after, or that names no curve before nor after (a curve given another
// parameter in two writes, its device and then its parameter, passing through one), failing it; once for each change of
// the curves' value; nothing of another area unless othersToo; and, where the tree told nothing else, the project's and the
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
        CHECK (each.told.curves == toldForTheTree (each));

        if (! othersToo)
        {
            CHECK (each.told.clips == 0);
            CHECK (each.told.notes == 0);
            CHECK (each.told.tempo == 0);
        }

        // Heard once: what listens to the song's root counts what it was told, and its tree's callbacks nothing more.
        if (each.other == 0)
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

TEST_CASE ("Song values' curves: what takes the curves along tells them, and what keeps them tells nothing: a device removed, a "
           "track duplicated, a folder made a bus and back, a version applied")
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

    // A device removed keeps its curves for an undo to bring it back (Automation.h): no curve changes.
    const auto removed = w.hearEach ("Remove Device", [&] { phazite::removeDevice (song, eq); });
    checkHeard ("Remove Device", removed);

    for (const auto& each : removed)
        CHECK (each.told.curves == 0);

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
        phazite::removeDevice (song, eq); // its curve kept, for an undo to bring it back
    }

    const auto after = phazite::test::copyWholeSong (song);
    REQUIRE (phazite::getCurveKeys (song) == std::vector { eqParameter, volume, pan });
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
    CHECK (phazite::getCurveKeys (song) == std::vector { eqParameter, volume, pan });
    CHECK_FALSE (phazite::test::hasDevice (song, eq));
    apply ("Without Automation", without);
    CHECK (phazite::getCurveKeys (song).empty());
    CHECK (song.getState().getChildWithName (phazite::schema::AUTOMATION).isValid());
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

TEST_CASE ("Song values' curves: a version without automation applied takes the song's curves away in one action, the song's "
           "elements staying in their places")
{
    // ApplySong.cpp: makeCurvesLike(), in one set of the curves' area. Three curves, and an element of the song's after its
    // AUTOMATION (its SECTIONS, added later), which keeps its place: the step is one action, not one for each curve, nor a
    // move of what comes after; what hears the curves change is told the three at once. The song's AUTOMATION, the place of
    // its curves, stays; a whole copy of the song, without curves, writes none, as the version has none.
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

    const auto state = song.getState();
    const auto automationAt = state.indexOf (state.getChildWithName (phazite::schema::AUTOMATION));
    REQUIRE (automationAt >= 0);
    REQUIRE (automationAt < state.indexOf (state.getChildWithName (phazite::schema::SECTIONS)));

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
    CHECK (state.indexOf (state.getChildWithName (phazite::schema::AUTOMATION)) == automationAt);
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

    // Deleted, its curves kept (for an undo to bring it back) and one of them changed since; put back from the version.
    undo.check ("Delete the Track",
                [&]
                {
                    const phazite::UndoStep step (song, "Delete the Track");
                    phazite::deleteTracks (song, { track }, nullptr);
                    phazite::addPoint (song, volume, { at (12.0), 0.1f });
                });
    REQUIRE_FALSE (phazite::findTrackState (song, track).isValid());
    restore ("Put Back the Track");
    REQUIRE (phazite::findTrackState (song, track).isValid());
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

    {
        const phazite::UndoStep step (song, "Edit");
        phazite::movePoints (song, volume, { 1 }, lasting (1.0), -0.2f);
        phazite::deletePoints (song, pan, { 0 });
    }

    phazite::undo (song);
    TreeHeard tree { song.getState() };
    phazite::makeSongAsCopied (song, copy);
    CHECK (tree.curves == 0);
    CHECK (tree.automationWithCurves == 0);
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

TEST_CASE ("Song values' curves: the keeper takes in a curve added to the song's AUTOMATION outside the history, its points given "
           "keys, and refuses one added in a step")
{
    // Its safety net (SongValues.h, CurvesArea.h), which the versions never leave anything to (ApplySong.cpp: NothingTakenIn).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& values = *song.getValues();
    const auto volume = s.getVolumeParameter (s.firstTrack), pan = s.getPanParameter (s.firstTrack);
    putPoints (song, volume, { { at (0.0), 0.5f } });
    const auto adoptedBefore = values.countCurvesAdopted();
    const auto element = [] (const phazite::ParameterRef& parameter, std::initializer_list<phazite::PointValue> points)
    {
        phazite::CurveValue curve;

        for (const auto& point : points)
            curve.points = curve.points.push_back (point);

        juce::ValueTree automation (phazite::schema::AUTOMATION);
        phazite::writeCurveElement (parameter, curve, automation);
        auto written = automation.getChild (0);
        automation.removeChild (written, nullptr);
        return written;
    };

    // A curve added outside the history: its points the song's, each with a key of its own, after the others; the element
    // taken out again.
    {
        const phazite::Song::OutsideHistory outside (song);
        song.getState()
            .getChildWithName (phazite::schema::AUTOMATION)
            .appendChild (
                element (pan, { { phazite::BeatPosition::fromBeats (1.0), 0.25f }, { phazite::BeatPosition::fromBeats (2.0), 0.75f } }),
                nullptr);
    }

    CHECK (phazite::getCurveKeys (song) == std::vector { volume, pan });
    REQUIRE (phazite::getPoints (song, pan).size() == 2);
    const auto keys = phazite::getPointKeys (song, pan);
    CHECK (keys[0] != 0);
    CHECK (keys[1] != 0);
    CHECK (keys[0] != keys[1]);
    CHECK (song.getState().getChildWithName (phazite::schema::AUTOMATION).getNumChildren() == 0);
    CHECK (values.countCurvesAdopted() == adoptedBefore + 1);
    phazite::test::checkSong (song);

    // A curve added by itself in a step: refused (automation's functions change the curves), the curves as they were; the
    // step undone takes it out again.
    const auto held = phazite::getCurvesValue (song);

    {
        const phazite::test::ExpectedAssertion refused ("CurvesArea.cpp");
        const phazite::UndoStep step (song, "A Lone Curve");
        song.getState()
            .getChildWithName (phazite::schema::AUTOMATION)
            .appendChild (element (volume, { { phazite::BeatPosition::fromBeats (4.0), 0.1f } }), &song.getUndoManager());
#if JUCE_DEBUG
        CHECK (refused.getCount() == 1);
#endif
    }

    CHECK (phazite::getCurvesValue (song).isSameAs (held));
    CHECK (values.countCurvesAdopted() == adoptedBefore + 1);
    phazite::undo (song);
    CHECK (song.getState().getChildWithName (phazite::schema::AUTOMATION).getNumChildren() == 0);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' curves: the keeper refuses an AUTOMATION added to the song, its curves unread, the song keeping its own, "
           "outside the history and in a step")
{
    // Every song that holds its values has its AUTOMATION from then on (CurvesArea.h: adopt()), which nothing adds or takes
    // out: another is a second, into which a whole copy would write the curves too. Refused, a JUCE assertion, in either mode:
    // taken out again outside the history; left in a step, whose Undo takes it out (JUCE workaround W62).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& values = *song.getValues();
    auto state = song.getState();
    const auto volume = s.getVolumeParameter (s.firstTrack), pan = s.getPanParameter (s.firstTrack);
    putPoints (song, volume, { { at (0.0), 0.5f } });
    const auto held = phazite::getCurvesValue (song);
    const auto adoptedBefore = values.countCurvesAdopted();
    const auto own = state.getChildWithName (phazite::schema::AUTOMATION);
    REQUIRE (own.isValid());

    // Another AUTOMATION with a curve in it.
    const auto second = [&pan]
    {
        phazite::CurveValue curve;
        curve.points = curve.points.push_back ({ phazite::BeatPosition::fromBeats (1.0), 0.25f });
        juce::ValueTree automation (phazite::schema::AUTOMATION);
        phazite::writeCurveElement (pan, curve, automation);
        return automation;
    };

    const auto countAutomations = [&state]
    {
        auto count = 0;

        for (const auto& child : state)
            count += child.hasType (phazite::schema::AUTOMATION) ? 1 : 0;

        return count;
    };

    // Outside the history: taken out again, the song's own kept, empty, and its curves as they were.
    {
        const phazite::test::ExpectedAssertion refused ("CurvesArea.cpp");
        const phazite::Song::OutsideHistory outside (song);
        state.appendChild (second(), nullptr);
#if JUCE_DEBUG
        CHECK (refused.getCount() == 1);
#endif
    }

    CHECK (countAutomations() == 1);
    CHECK (state.getChildWithName (phazite::schema::AUTOMATION) == own);
    CHECK (own.getNumChildren() == 0);
    CHECK (phazite::getCurvesValue (song).isSameAs (held));
    CHECK (values.countCurvesAdopted() == adoptedBefore);
    phazite::test::checkSong (song);

    // In a step: left to the step's Undo, which takes it out, the curves as they were throughout; the check finds the
    // second meanwhile.
    {
        const phazite::test::ExpectedAssertion refused ("CurvesArea.cpp");
        const phazite::UndoStep step (song, "A Second Automation");
        state.appendChild (second(), &song.getUndoManager());
#if JUCE_DEBUG
        CHECK (refused.getCount() == 1);
#endif
    }

    CHECK (countAutomations() == 2);
    CHECK (phazite::getCurvesValue (song).isSameAs (held));
    CHECK (values.countCurvesAdopted() == adoptedBefore);
    const auto problems = phazite::test::findValueProblems (song);
    CHECK (std::find (problems.begin(), problems.end(), "values: the song has more than one AUTOMATION") != problems.end());
    phazite::undo (song);
    CHECK (countAutomations() == 1);
    CHECK (state.getChildWithName (phazite::schema::AUTOMATION) == own);
    CHECK (phazite::getCurvesValue (song).isSameAs (held));
    phazite::test::checkSong (song);
}
