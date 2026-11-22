// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's automation change hears it from one place (model/base/internal/SongValues.h:
// Listener::curvesChanged()), by the parameters of the curves that changed (roadmap step S5.20). For each edit of the curves
// made through the model, its Undo and its Redo, the listener is told exactly the parameters whose curves differ before and
// after, compared as values (independent of what tells it), and nothing of another area; another area's change tells it
// nothing; and what listens to the song's root hears each change once: the project's count of changes and the versions'
// count of the song's changes go up by what the listener was told, and by nothing else, where the tree told nothing but the
// curves.
//   phazite_tests.exe --test-case="Song values' curves*"
#include "EnginePlayback.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Arrangement.h"
#include "model/arrangement/internal/Clips.h"
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
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <array>
#include <functional>
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

// The parameters whose curves differ between two values of the song's curves: one in one and not the other, or with other
// points. Independent of what the listener is told.
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
        int curves = 0, automationWithCurves = 0, other = 0;
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
        const auto projectBefore = s.getProject().getChangeCount();
        const auto versionsBefore = versions->countSongChanges();
        const auto before = phazite::getCurvesValue (song);
        change();
        const auto after = phazite::getCurvesValue (song);
        return { told,
                 tree.curves,
                 tree.automationWithCurves,
                 tree.other,
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
};

// How often the listener is told for what the tree told: while the song's tree holds the curves, the keeper tells it once
// for each of the tree's notifications under them, and once for an AUTOMATION added or taken out with curves in it. At
// automation's switch (roadmap step S5.20) the area tells it once for each change of its value: this is the one place that
// changes then.
int toldForTheTree (const Watched::Heard& heard)
{
    return heard.curves + heard.automationWithCurves;
}

// What a change and its Undo and Redo told, each against the parameters whose curves differ: exactly those, but where a
// parameter that names no curve before nor after may be told too (alsoGone: a curve given another parameter in two writes,
// its device and then its parameter, passes through one, on the tree; renameCurve() of roadmap step S5.20's next piece
// gives it its new parameter in one); a parameter told whose curve is there and the same before and after fails it, so
// does one told with no curve where alsoGone is not set; once as often as the tree changed under the curves; nothing of
// another area unless othersToo; and, where the tree told nothing else, the project's and the versions' counts up by what
// the listener was told.
void checkHeard (const juce::String& name, const std::vector<Watched::Heard>& heard, bool othersToo = false, bool alsoGone = false)
{
    INFO (name.toStdString());
    const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
    REQUIRE (heard.size() <= moments.size());

    for (size_t i = 0; i < heard.size(); ++i)
    {
        INFO (moments[i]);
        const auto& each = heard[i];
        auto told = each.told.parameters;

        if (alsoGone)
            std::erase_if (told, [&each] (const phazite::ParameterRef& parameter)
                           { return each.before.find (parameter) == nullptr && each.after.find (parameter) == nullptr; });

        CHECK_MESSAGE (describe (told) == describe (each.differing), "told " << describe (each.told.parameters).joinIntoString (", ")
                                                                             << "; differing "
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
    REQUIRE (phazite::findDeviceState (song, eq).isValid());

    // A track duplicated with its fader's and its devices' curves: the copies' curves, under their new IDs.
    const auto duplicated = w.hearEach ("Duplicate Track", [&] { phazite::duplicateTracks (song, { track }, nullptr); });
    checkHeard ("Duplicate Track", duplicated, true);
    CHECK (duplicated.front().differing.size() == 3); // the copy's pan, volume and EQ's

    // A folder's volume curve given to the fader that takes its place, each way: the parameter it had and the one it has.
    const auto bus = w.s.addBus ("Bus", { track });
    phazite::addAutomationPoint (w.s.context, w.s.getVolumeParameter (bus), { at (1.0), 0.6f });
    const auto toPlain = w.hearEach ("Make Plain Folder", [&] { phazite::makePlainFolder (song, bus); });
    checkHeard ("Make Plain Folder", toPlain, true, true);
    CHECK (toPlain.front().differing.size() == 2);
    checkHeard ("Make Bus", w.hearEach ("Make Bus", [&] { phazite::makeBus (song, bus); }), true, true);

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
