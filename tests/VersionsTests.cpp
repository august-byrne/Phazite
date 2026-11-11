// SPDX-License-Identifier: AGPL-3.0-or-later
// A project's versions (docs/versions.md): saving them, reading them back as the project opens, and the changes since the
// last one, by track, in Phazite's own words.
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/modulation/Modulators.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/VersionHistory.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/DevicesView.h"

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

void waitForVersions (phazite::ProjectContext& context)
{
    REQUIRE (dispatchMessagesUntil (
        [&context]
        {
            const auto* history = phazite::getVersionHistory (context);
            return history == nullptr || ! history->isBusy();
        }));
}

// Saves a version with a name and a note, and waits until it is written.
void keepVersion (phazite::ProjectContext& context, const juce::String& name = {}, const juce::String& note = {})
{
    waitForVersions (context);
    phazite::setVersionDraft (context, name, note);
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }));
    INFO (result->getErrorMessage().toStdString());
    REQUIRE (result->wasOk());
    waitForVersions (context);
}

juce::String describe (const std::vector<phazite::VersionChange>& changes)
{
    juce::StringArray lines;

    for (const auto& change : changes)
        lines.add ((change.kind == phazite::VersionChange::Kind::added     ? "+ "
                    : change.kind == phazite::VersionChange::Kind::removed ? "- "
                                                                           : "")
                   + change.name + (change.what.isNotEmpty() ? ": " + change.what : juce::String()));

    return lines.joinIntoString ("; ");
}

juce::String describeChanges (phazite::ProjectContext& context)
{
    return describe (phazite::getChangesSinceLastVersion (context));
}

// The versions' commands as the app registers them (model/versions/VersionsModel.h), for a test's song, whose saves always
// succeed; each run as a menu runs it, which must be enabled.
struct VersionCommands
{
    explicit VersionCommands (TestSong& s)
    {
        phazite::addVersionCommands (registry, s.context, [] (std::function<void (bool)> then) { then (true); }, s.renders);
    }

    void run (const juce::String& id) { REQUIRE_MESSAGE (registry.invoke (id), (id + " is unknown or greyed out").toStdString()); }

    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("A project starts with its initial commit on main, and lists each commit after it")
{
    TestSong s;
    waitForVersions (s.context);
    CHECK (phazite::keepsVersions (s.context));
    const auto initial = phazite::getVersions (s.context);
    REQUIRE (initial.size() == 1);
    CHECK (initial[0].name == "Initial commit");
    CHECK (initial[0].parents.isEmpty());
    CHECK (phazite::getCurrentBranch (s.context)->name == "main");
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (s.getProject().getFolder().getChildFile ("Versions").isDirectory());

    keepVersion (s.context, "First idea", "Drums only, for now");

    const auto versions = phazite::getVersions (s.context);
    REQUIRE (versions.size() == 2);
    CHECK (versions[0].name == "First idea");
    CHECK (versions[0].note == "Drums only, for now");
    CHECK (versions[0].hasOwnName);
    CHECK (versions[0].parents == juce::StringArray (initial[0].id));

    const auto branch = phazite::getCurrentBranch (s.context);
    REQUIRE (branch.has_value());
    CHECK (branch->name == "main");
    CHECK (branch->head == versions[0].id);
    CHECK (versions[0].branch == branch->id);
    CHECK (phazite::getLastVersion (s.context)->id == versions[0].id);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (phazite::getVersionDraftName (s.context).isEmpty()); // the draft went with the version
    CHECK (phazite::getVersionsSize (s.context) > 0);
}

TEST_CASE ("The changes since the last version name each track's, in Phazite's own words")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto synth = s.setInstrument (bass, phazite::devices::Synth::typeID);
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    s.addNote (clip, 38, 1.0, 1.0);
    keepVersion (s.context, "Verse");
    CHECK (describeChanges (s.context).isEmpty());

    s.setVolume (bass, -6.0f);
    CHECK (describeChanges (s.context) == "Bass: volume");

    s.addNote (clip, 40, 2.0, 1.0);
    s.addNote (clip, 41, 3.0, 1.0);
    CHECK (describeChanges (s.context) == "Bass: volume, 2 notes");

    s.setParameter (synth, "cutoff", 1200.0f);
    CHECK (describeChanges (s.context) == "Bass: volume, Synth, 2 notes");

    const auto eq = s.addEffect (bass, phazite::devices::Equalizer::typeID);
    CHECK (describeChanges (s.context) == "Bass: volume, Synth, + EQ, 2 notes");

    const auto strings = s.addTrack ("Strings");
    CHECK (describeChanges (s.context) == "Bass: volume, Synth, + EQ, 2 notes; + Strings");

    s.addAutomation ({ eq, "band1Gain" }, { { phazite::BeatPosition(), 0.5f }, { phazite::BeatPosition::fromBeats (4.0), 0.7f } });
    CHECK (describeChanges (s.context).startsWith ("Bass: volume, Synth, + EQ, 2 notes, automation"));

    s.setTempo (128.0);
    CHECK (describeChanges (s.context).endsWith ("; Song: tempo"));

    // Committed without a message, the commit is named by what changed, as git's messages are written.
    keepVersion (s.context);
    CHECK (phazite::getVersions (s.context).front().name == "Add Strings; update Bass and tempo");
    CHECK_FALSE (phazite::getVersions (s.context).front().hasOwnName);
    CHECK (describeChanges (s.context).isEmpty());
    juce::ignoreUnused (strings);
}

TEST_CASE ("View state and how the song plays are no change since the last version")
{
    TestSong s;
    const auto track = s.addTrack ("Keys");
    keepVersion (s.context);

    auto& song = *phazite::findSong (s.context);
    auto state = phazite::findTrackState (song, track);
    state.setProperty (schema::trackHeight, 120, nullptr);
    state.setProperty (schema::trackCollapsed, true, nullptr);
    song.getState().setProperty (schema::loop, true, nullptr);
    song.getState().setProperty (schema::metronome, true, nullptr);
    song.getState().setProperty (schema::gridStep, "1 bar", nullptr);

    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // An undo back to where the version was is no change either.
    s.setVolume (track, -3.0f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::undo (song);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
}

TEST_CASE ("A knob turned and set back to its default, or a modulator's, is no change since the last version, nor unsaved")
{
    // As a double-click sets a knob back (known bug 9 until 2026-11-06: the default stayed in the device's values).
    TestSong s;
    const auto track = s.addTrack ("Keys");
    const auto compressor = s.addEffect (track, phazite::devices::Compressor::typeID);
    const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
    const auto follower = s.addModulator (track, phazite::ModulatorType::envelopeFollower);
    const auto defaultOf = [] (const auto& parameters, std::string_view parameter)
    {
        for (const auto& info : parameters)
            if (info.id == parameter)
                return info.defaultValue;

        FAIL ("no parameter " << parameter);
        return 0.0f;
    };
    const auto& compressorParameters = phazite::devices::Compressor::info.parameters;
    s.setParameter (compressor, "attack", 30.0f); // a value set before the version, which stays as it was
    keepVersion (s.context);
    REQUIRE (s.getProject().save().wasOk());
    dispatchPendingMessages();
    REQUIRE_FALSE (s.getProject().hasUnsavedChanges());
    auto& song = *phazite::findSong (s.context);
    const auto values = [&song, compressor] { return phazite::findDeviceState (song, compressor).getChildWithName (schema::VALUES); };
    REQUIRE (values().getNumProperties() == 1);

    s.setParameter (compressor, "inputGain", 6.0f);
    CHECK (describeChanges (s.context) == "Keys: Compressor");
    s.setParameter (compressor, "inputGain", defaultOf (compressorParameters, "inputGain"));
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK (values().getNumProperties() == 1);

    // Its mix too; and the value set before the version set back, which leaves the device no values at all, as it was made.
    phazite::setMix (song, compressor, 0.5f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::setMix (song, compressor, 1.0f);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    s.setParameter (compressor, "attack", defaultOf (compressorParameters, "attack"));
    CHECK_FALSE (values().isValid());
    CHECK (describeChanges (s.context) == "Keys: Compressor");
    phazite::undo (song);
    CHECK ((float) values()["attack"] == doctest::Approx (30.0f));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // A modulator's parameter: an LFO's rate, and back; and an envelope follower's depth, which it starts at, its default.
    s.setModulatorParameter (lfo, "rate", 5.0f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    s.setModulatorParameter (lfo, "rate",
                             defaultOf (phazite::devices::getModulatorParameters (phazite::devices::ModulatorKind::lfo), "rate"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    s.setModulatorParameter (follower, "depth", 0.5f);
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    s.setModulatorParameter (
        follower, "depth",
        defaultOf (phazite::devices::getModulatorParameters (phazite::devices::ModulatorKind::envelopeFollower), "depth"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    // The faders too, the master's to where it starts, a little down, which its double-click sets (it set 0 dB: 2026-11-09).
    const auto setBack = [&s] (phazite::ParameterHandle fader)
    {
        INFO (fader.getName().toStdString());
        REQUIRE (fader.getDefaultValue().has_value());
        fader.setValue (0.3f);
        CHECK (phazite::hasChangesSinceLastVersion (s.context));
        fader.setValue (*fader.getDefaultValue());
        CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
        CHECK_FALSE (s.getProject().hasUnsavedChanges());
    };

    setBack (phazite::getVolume (s.context, track));
    setBack (phazite::getMasterVolume (s.context));
}

TEST_CASE ("An effect's knob turned in the Devices view, then double-clicked back or undone, is no change since the last version")
{
    // As the user sets a knob back (the owner's report, 2026-11-09): dragged, then double-clicked, which sets it to its
    // default; or dragged, then undone. Each of Phazite's own effects in its editor beside the column, their mixes at their
    // lines' ends, and a plug-in's key knobs, which set its parameters through Phazite's controls.
    TestPlugin testPlugin; // before the song that holds one, and gone after it
    TestSong s;
    const auto track = s.addTrack ("Effects");
    std::vector<std::pair<phazite::DeviceID, juce::String>> effects;

    for (const auto& type : phazite::getDeviceTypes (s.context))
        if (! type.isInstrument && (type.isBuiltIn || type.description.fileOrIdentifier == TestPlugin::getDescription().fileOrIdentifier))
            effects.emplace_back (s.addEffect (track, type.description.fileOrIdentifier.toRawUTF8()), type.name);

    REQUIRE (effects.size() > 10);
    keepVersion (s.context);
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    auto& song = *phazite::findSong (s.context);

    phazite::ui::CommandBridge bridge { s.commands };
    phazite::ui::DevicesView view { bridge, s.context };
    TestWindow window { view, 1600, 380 };
    s.context.getSongSelection().selectOnly (track);
    dispatchPendingMessages();
    Mouse mouse (view); // one for every knob, so that its clicks' times only go forward

    // The knobs that show, enabled, where the mouse reaches them (not scrolled away under the column's top): in what shows
    // the device beside the column, or in the column.
    const auto findKnobs = [&view] (bool beside)
    {
        std::vector<phazite::ui::Knob*> knobs;
        auto* body = view.getShownBody();

        for (auto* knob : findAll<phazite::ui::Knob> (view))
        {
            auto* reached = view.getComponentAt (view.getLocalPoint (knob, knob->getLocalBounds().getCentre()));

            if (knob->isShowing() && knob->isEnabled() && (reached == knob || knob->isParentOf (reached))
                && (body != nullptr && body->isParentOf (knob)) == beside)
                knobs.push_back (knob);
        }

        return knobs;
    };

    // A drag up, or down from the top of its range; where it was pressed.
    const auto turn = [&view, &mouse] (phazite::ui::Knob& knob)
    {
        const auto centre = view.getLocalPoint (&knob, knob.getLocalBounds().getCentre());
        const auto atTop = knob.getValue() >= knob.getMaximum() - 0.001 * (knob.getMaximum() - knob.getMinimum());
        mouse.press (centre);

        for (int step = 1; step <= 4; ++step)
            mouse.dragTo (centre.translated (0, (atTop ? 40 : -40) * step / 4));

        mouse.release();
        mouse.pause();
        return centre;
    };

    // Each knob found again after each change, which may make the view's parts anew.
    const auto turnAndSetBack = [&] (const juce::String& device, bool beside)
    {
        const auto count = findKnobs (beside).size();

        for (size_t i = 0; i < count; ++i)
        {
            const auto knob = [&findKnobs, beside, count, i]
            {
                const auto knobs = findKnobs (beside);
                REQUIRE (knobs.size() == count);
                return knobs[i];
            };

            const auto what = device + ": " + knob()->getTitle();
            INFO (what.toStdString());
            const auto centre = turn (*knob());
            CHECK (phazite::hasChangesSinceLastVersion (s.context));
            mouse.doubleClick (centre);
            dispatchPendingMessages();
            CHECK (knob()->getValue() == doctest::Approx (knob()->getDoubleClickReturnValue()));
            CHECK (describeChanges (s.context) == "");

            turn (*knob());
            CHECK (phazite::hasChangesSinceLastVersion (s.context));
            phazite::undo (song);
            dispatchPendingMessages();
            CHECK (describeChanges (s.context) == "");
        }

        return count;
    };

    for (const auto& [device, name] : effects)
    {
        view.showDevice (device);
        dispatchPendingMessages();
        REQUIRE (view.getShownBody() != nullptr);
        CHECK (turnAndSetBack (name, true) > 0);
    }

    CHECK (turnAndSetBack ("Mix", false) > 0);

    // A plug-in's knob turned in its own window and set back there, its settings stored after each, as Auto Save stores them.
    const auto plugin = effects.back().first; // the stand-in: plug-ins come after Phazite's own
    const auto gain = TestPlugin::getGain (song, plugin);
    TestPlugin::setGain (song, plugin, 0.25f);
    song.storeDeviceStates();
    CHECK (describeChanges (s.context).contains ("Gain"));
    TestPlugin::setGain (song, plugin, gain);
    song.storeDeviceStates();
    CHECK (describeChanges (s.context) == "");
}

TEST_CASE ("A plug-in's knob set to its default by Phazite's controls keeps no value in the song, and what it plays still saves")
{
    TestPlugin testPlugin; // before the song that holds one, and gone after it
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    phazite::DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Add the stand-in");
        plugin = TestPlugin::add (song, s.firstTrack);
    }

    // Its Gain at a quarter, set in its own window, which its settings hold: the song keeps no value of its own for it.
    TestPlugin::setGain (song, plugin, 0.25f);
    keepVersion (s.context);
    REQUIRE (s.getProject().save().wasOk());
    dispatchPendingMessages();
    REQUIRE_FALSE (s.getProject().hasUnsavedChanges());
    const auto hasValue = [&song, plugin]
    { return phazite::findDeviceState (song, plugin).getChildWithProperty (schema::valueParameter, TestPlugin::gainID).isValid(); };
    const auto played = [&song, plugin]
    {
        auto* instance = phazite::findPluginInstance (song, plugin);
        REQUIRE (instance != nullptr);
        return instance->getParameterValue (instance->findParameter (TestPlugin::gainID));
    };
    REQUIRE_FALSE (hasValue());

    // Set to its default by a control, as a double-click sets it: a change from what the plug-in held, which the song keeps.
    s.setParameter (plugin, TestPlugin::gainID, 1.0f);
    CHECK (played() == doctest::Approx (1.0f));
    CHECK (hasValue());
    CHECK (describeChanges (s.context).contains ("Gain 25%"));

    // Turned and set back to its default again: no value in the song, the plug-in holding it, and still a change that saves.
    s.setParameter (plugin, TestPlugin::gainID, 0.5f);
    s.setParameter (plugin, TestPlugin::gainID, 1.0f);
    CHECK_FALSE (hasValue());
    CHECK (played() == doctest::Approx (1.0f));
    CHECK (describeChanges (s.context).contains ("Gain 25%"));
    CHECK (s.getProject().hasUnsavedChanges());

    // Undone, step by step, back to what the plug-in held, never through 0, and no change at all.
    phazite::undo (song);
    CHECK (played() == doctest::Approx (0.5f));
    phazite::undo (song);
    CHECK (played() == doctest::Approx (1.0f));
    phazite::undo (song);
    CHECK (played() == doctest::Approx (0.25f));
    CHECK_FALSE (hasValue());
    CHECK (describeChanges (s.context) == "");

    // Redone and saved: opened again, the plug-in plays its default from its settings.
    phazite::redo (song);
    phazite::redo (song);
    phazite::redo (song);
    CHECK_FALSE (hasValue());
    REQUIRE (s.getProject().save().wasOk());
    s.reopen();
    CHECK (TestPlugin::getGain (*phazite::findSong (s.context), plugin) == doctest::Approx (1.0f));
}

TEST_CASE ("A track muted and unmuted again, soloed and unsoloed, or a clip muted and unmuted, is no change since the last version")
{
    TestSong s;
    const auto track = s.addTrack ("Keys");
    const auto other = s.addTrack ("Pad");
    const auto clip = s.addMidiClip (track, 0.0, 4.0);
    keepVersion (s.context);
    auto& song = *phazite::findSong (s.context);

    // Muted, it is a change; unmuted, the track is as the version has it, so the change goes (it stayed: 2026-11-06).
    phazite::toggleTracksMuted (song, { track });
    CHECK (describeChanges (s.context) == "Keys: mute");
    phazite::toggleTracksMuted (song, { track });
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // So do a solo, alone or not, and a clip's mute.
    phazite::toggleTracksSoloed (song, { track });
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleTracksSoloed (song, { track });
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleSoloAlone (song, other);
    phazite::toggleSoloAlone (song, other);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleClipsMuted (song, { clip });
    CHECK (phazite::hasChangesSinceLastVersion (s.context));
    phazite::toggleClipsMuted (song, { clip });
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
}

TEST_CASE ("Collapse All and Expand All, the master's row and lanes with the rest, are no change since the last version, nor unsaved")
{
    // The owner, 2026-11-06: Alt+U (Collapse All) in the playlist left a change of the master's, which Expand All didn't
    // take away.
    TestSong s;
    s.addTrack ("Keys");
    auto& song = *phazite::findSong (s.context);
    const auto master = phazite::getMasterID (song);
    phazite::showLane (song, master, { phazite::getFader (song, master), phazite::getVolumeParameterID (song, master) });
    keepVersion (s.context);
    REQUIRE (s.getProject().save().wasOk());
    dispatchPendingMessages();
    REQUIRE_FALSE (s.getProject().hasUnsavedChanges());

    s.run ("track.collapseAll");
    REQUIRE (phazite::isCollapsed (song, master));
    REQUIRE_FALSE (phazite::isAnyRowExpanded (song));
    dispatchPendingMessages();
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    s.run ("track.collapseAll");
    REQUIRE_FALSE (phazite::isCollapsed (song, master));
    dispatchPendingMessages();
    CHECK (describeChanges (s.context).isEmpty());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
}

TEST_CASE ("A project's versions are read again as it opens, with the song each one kept")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "One");
    const auto firstSong = phazite::findSong (s.context)->getState().createCopy();

    s.setVolume (bass, -10.0f);
    keepVersion (s.context, "Two");
    s.setVolume (bass, -20.0f); // since the last version, and saved with the project

    s.reopen();
    waitForVersions (s.context);

    const auto versions = phazite::getVersions (s.context);
    REQUIRE (versions.size() == 3);
    CHECK (versions[0].name == "Two");
    CHECK (versions[1].name == "One");
    CHECK (versions[2].name == "Initial commit");
    CHECK (versions[0].parents == juce::StringArray (versions[1].id));
    CHECK (phazite::getCurrentBranch (s.context)->head == versions[0].id);
    CHECK (describeChanges (s.context) == "Bass: volume");

    // The first version's song, as it was.
    auto* history = phazite::getVersionHistory (s.context);
    REQUIRE (history != nullptr);
    const auto read = history->readSong (versions[1].id);
    REQUIRE (read.isValid());
    CHECK (phazite::holdTheSame (read, firstSong));
    CHECK (phazite::findChanges (read, firstSong).empty());
}

TEST_CASE ("Commit saves the project first, then commits")
{
    TestSong s;
    phazite::CommandRegistry commands;
    int saves = 0;

    phazite::addVersionCommands (
        commands, s.context,
        [&s, &saves] (std::function<void (bool)> then)
        {
            ++saves;
            then (s.getProject().save().wasOk());
        },
        s.renders);

    waitForVersions (s.context);
    s.addTrack ("Lead");
    REQUIRE (s.getProject().hasUnsavedChanges());
    REQUIRE (commands.invoke ("version.save"));
    waitForVersions (s.context);

    CHECK (saves == 1);
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK (phazite::getVersions (s.context).size() == 2);
    CHECK_FALSE (phazite::getVersions (s.context).front().hasOwnName);
    CHECK (phazite::getVersions (s.context).front().name == "Add Lead");
}

TEST_CASE ("A copy of a project takes its versions with it")
{
    TestSong s;
    s.addTrack ("Pad");
    keepVersion (s.context, "Before the copy");

    juce::String error;
    const auto copy = s.getProject().saveCopyAs (s.getProject().getFolder().getParentDirectory(), "Copy", error);
    INFO (error.toStdString());
    REQUIRE (copy.existsAsFile());

    phazite::ProjectContext context { getEngineContext() };
    auto project = phazite::Project::open (copy, false, error);
    REQUIRE (project != nullptr);
    phazite::setSong (context, &project->getSong());
    waitForVersions (context);

    const auto versions = phazite::getVersions (context);
    REQUIRE (versions.size() == 2);
    CHECK (versions[0].name == "Before the copy");
    phazite::setSong (context, nullptr);
}

//==============================================================================
TEST_CASE ("The differences between two songs match items by their IDs, wherever they went")
{
    using phazite::findChanges;
    auto before = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*before, 2);
    phazite::setTrackName (*before, tracks[0], "Drums");
    const auto then = before->getState().createCopy();

    // The second track moved above the first: the song's order, no track's own change.
    auto list = phazite::getTrackList (*before);
    const auto second = phazite::findTrackState (*before, tracks[1]);
    list.moveChild (list.indexOf (second), 0, nullptr);
    const auto moved = findChanges (then, before->getState());
    REQUIRE (moved.size() == 1);
    CHECK (moved[0].where == phazite::SongChange::Where::song);
    CHECK (moved[0].what == juce::StringArray ("track order"));

    // The same song: nothing.
    CHECK (findChanges (then, then.createCopy()).empty());
    CHECK (phazite::holdTheSame (then, then.createCopy()));

    // A plug-in's knobs named by the values it told.
    const phazite::PluginParameterValues valuesThen { { 7, { { "1", "Cutoff", 0.4f }, { "2", "Drive", 0.1f } } } };
    const phazite::PluginParameterValues valuesNow { { 7, { { "1", "Cutoff", 0.55f }, { "2", "Drive", 0.1f } } } };
    const juce::ValueTree plugin (
        schema::DEVICE, { { schema::deviceID, 7 }, { schema::deviceType, schema::deviceIsPlugin }, { schema::pluginName, "Serum" } });
    const juce::ValueTree trackThen (schema::TRACK, { { schema::trackID, 3 }, { schema::trackName, "Lead" } }, { plugin.createCopy() });
    const juce::ValueTree trackNow (schema::TRACK, { { schema::trackID, 3 }, { schema::trackName, "Lead" } }, { plugin.createCopy() });
    const auto change = phazite::findTrackChange (trackThen, trackNow, {}, {}, { &valuesThen, &valuesNow });
    CHECK (change.what == juce::StringArray (juce::String ("Serum: Cutoff 40%") + juce::String::fromUTF8 (" \xe2\x86\x92 ") + "55%"));
}

TEST_CASE ("A commit without a message gets one as git's are written, from its changes, a few of them")
{
    using Change = phazite::SongChange;
    const std::vector<Change> changes { { Change::Where::track, Change::Kind::changed, 1, "Bass", { "volume" } },
                                        { Change::Where::track, Change::Kind::changed, 2, "Drums", { "2 clips added" } },
                                        { Change::Where::track, Change::Kind::added, 3, "Strings", {} },
                                        { Change::Where::track, Change::Kind::removed, 4, "Pad", {} },
                                        { Change::Where::song, Change::Kind::changed, 0, "Song", { "tempo", "sections" } } };

    CHECK (phazite::summarise ({ changes.begin(), changes.begin() + 3 }) == "Add Strings; update Bass and Drums");
    CHECK (phazite::summarise (changes) == "Add Strings; update Bass, Drums, tempo and sections; remove Pad");
    CHECK (phazite::summarise ({}) == "No changes");
}

TEST_CASE ("Staging, as git's: the staged changes are what a commit records, and the changes what changed since they were staged")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    keepVersion (s.context, "Start");
    const auto volume = [&s] (phazite::TrackID track) { return phazite::getChannelVolume (*phazite::findSong (s.context), track); };

    s.setVolume (bass, -6.0f);
    s.setVolume (drums, -9.0f);
    CHECK (describeChanges (s.context) == "Bass: volume; Drums: volume");
    CHECK (phazite::getStagedChanges (s.context).empty());
    CHECK (phazite::countChanges (s.context) == 2);

    // Bass staged: in the staged changes, no more in the changes; Drums still to stage.
    REQUIRE (phazite::stageChange (s.context, phazite::getChangesSinceLastVersion (s.context)[0]));
    auto staged = phazite::getStagedChanges (s.context);
    REQUIRE (staged.size() == 1);
    CHECK (staged[0].name == "Bass");
    CHECK (staged[0].staged);
    CHECK (describeChanges (s.context) == "Drums: volume");
    CHECK (phazite::countChanges (s.context) == 2);

    // Changed again once staged: in both, as in git; Discard Changes takes it back to what is staged, as git restore does.
    s.setVolume (bass, -3.0f);
    CHECK (describeChanges (s.context) == "Bass: volume; Drums: volume");
    REQUIRE (phazite::takeBack (s.context, phazite::getChangesSinceLastVersion (s.context)[0]));
    CHECK (volume (bass) == doctest::Approx (-6.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard Changes in Bass");
    CHECK (describeChanges (s.context) == "Drums: volume");

    // A commit records what is staged alone: Drums stays a change since it.
    auto* history = phazite::getVersionHistory (s.context);
    const auto start = phazite::getLastVersion (s.context)->id;
    keepVersion (s.context, "Bass down");
    CHECK (phazite::getStagedChanges (s.context).empty());
    CHECK (describeChanges (s.context) == "Drums: volume");
    CHECK (describe (phazite::getChangesSinceLastVersion (s.context)) == "Drums: volume");
    const auto committed = phazite::findChanges (history->readSong (start), history->readSong (phazite::getLastVersion (s.context)->id));
    REQUIRE (committed.size() == 1);
    CHECK (committed[0].name == "Bass");

    // Unstaged again: the line back in the changes; every line at once.
    REQUIRE (phazite::stageChange (s.context, phazite::getChangesSinceLastVersion (s.context)[0]));
    REQUIRE (phazite::unstageChange (s.context, phazite::getStagedChanges (s.context)[0]));
    CHECK (phazite::getStagedChanges (s.context).empty());
    CHECK (describeChanges (s.context) == "Drums: volume");
    phazite::stageAllChanges (s.context);
    CHECK (phazite::getChangesSinceLastVersion (s.context).empty());
    CHECK (phazite::getStagedChanges (s.context).size() == 1);
    phazite::unstageAllChanges (s.context);
    CHECK (phazite::getStagedChanges (s.context).empty());

    // With nothing staged, a commit takes every change, as git's smart commit.
    keepVersion (s.context, "Drums down");
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // Discard All Changes: the song as committed again, one undo step.
    s.setVolume (bass, 0.0f);
    s.addTrack ("Keys");
    REQUIRE (phazite::discardAllChanges (s.context));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard All Changes");
}

TEST_CASE ("A branch's name is what git takes: spaces as dashes, without what a ref can't hold")
{
    CHECK (phazite::toBranchName ("Half time") == "Half-time");
    CHECK (phazite::toBranchName ("  big   chorus ") == "big-chorus");
    CHECK (phazite::toBranchName ("what?*[now]:~^") == "whatnow]");
    CHECK (phazite::toBranchName ("../.hidden.lock") == "hidden");
    CHECK (phazite::toBranchName ("feature//new idea/") == "feature/new-idea");
    CHECK (phazite::toBranchName ("---") == "");
}

TEST_CASE ("A plug-in's stored settings that differ count as a change only after it said it changed, or a knob of it moved")
{
    phazite::test::TestPlugin testPlugin; // known while the song is
    TestSong s;
    const auto track = s.addTrack ("Effects");
    auto& song = *phazite::findSong (s.context);
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (song, "Add the stand-in");
        device = phazite::test::TestPlugin::add (song, track);
    }

    keepVersion (s.context, "First");
    CHECK (describeChanges (s.context).isEmpty());

    // Other bytes stored with nothing said, as a plug-in that writes a time into its state leaves them at each save.
    phazite::findDeviceState (song, device)
        .setProperty (schema::pluginState, phazite::writeBytes (juce::MemoryBlock ("other", 5)), nullptr);
    dispatchPendingMessages();
    CHECK (describeChanges (s.context).isEmpty());

    // Once it said its settings changed, they count.
    song.markChangedOutsideHistory (device.getRaw());
    CHECK (describeChanges (s.context).contains ("Effects"));
}

TEST_CASE ("A plug-in's update shows in the changes, by the version that made its settings then and now")
{
    phazite::test::TestPlugin testPlugin;
    TestSong s;
    const auto track = s.addTrack ("Effects");
    auto& song = *phazite::findSong (s.context);
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (song, "Add the stand-in");
        device = phazite::test::TestPlugin::add (song, track);
    }

    // A commit stores the version of the plug-in that made the state, as it stores the state.
    phazite::findDeviceState (song, device).setProperty (schema::pluginVersion, "1.0.0", nullptr);
    keepVersion (s.context, "First");
    const auto then = phazite::findDeviceState (song, device)[schema::pluginVersion].toString();
    const auto installed = phazite::test::TestPlugin::getDescription().version;
    CHECK (then == (installed.isNotEmpty() ? installed : juce::String ("1.0.0")));

    phazite::findDeviceState (song, device).setProperty (schema::pluginVersion, "9.9.9", nullptr);
    CHECK (describeChanges (s.context).contains ("updated " + then + juce::String::fromUTF8 (" \xe2\x86\x92 9.9.9")));
}

TEST_CASE ("Verify History finds the commits no branch holds and what is damaged, and Recover Lost Commits makes a branch of each line")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto* history = phazite::getVersionHistory (s.context);
    const auto first = history->findLastVersion()->id;

    // A commit that no branch holds, as a lost branch file would leave one: written into the store beside the others.
    juce::String lostID;

    {
        phazite::VersionStore store (phazite::getVersionsFolder (song), "elsewhere");
        phazite::VersionRecord lost;
        lost.name = "Lost";
        lost.parents.add (first);
        lost.branch = history->findCurrentBranch()->id;
        lost.saved = juce::Time::currentTimeMillis();
        lostID = store.writeVersion (lost, phazite::splitSong (song.getState()));
        REQUIRE (lostID.isNotEmpty());
    }

    const auto verify = [&s]
    {
        REQUIRE (phazite::verifyHistory (s.context));
        REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::isVerifyingHistory (s.context); }, 20000));
    };

    verify();
    CHECK (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    CHECK (phazite::countLostVersions (s.context) == 1);
    CHECK (phazite::describeHistoryCheck (s.context).contains ("Recover Lost Commits"));

    // Recovered: a branch at it, named after the branch it was made on.
    CHECK (phazite::recoverLostVersions (s.context) == 1);
    CHECK (phazite::countLostVersions (s.context) == 0);
    const auto branches = phazite::getBranches (s.context);
    CHECK (std::any_of (branches.begin(), branches.end(), [&lostID] (const phazite::BranchInfo& branch)
                        { return branch.name == "main-recovered" && branch.head == lostID; }));

    // A byte of a pack damaged: the piece is found, and the commits that need it named.
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Second");
    juce::File newest;

    for (const auto& pack :
         phazite::getVersionsFolder (song).getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack"))
        if (newest == juce::File() || pack.getLastModificationTime() > newest.getLastModificationTime())
            newest = pack;

    juce::MemoryBlock bytes;
    REQUIRE (newest.loadFileAsData (bytes));
    static_cast<char*> (bytes.getData())[10] ^= 0x5a;
    REQUIRE (newest.replaceWithData (bytes.getData(), bytes.getSize()));

    verify();
    CHECK (phazite::describeHistoryCheck (s.context).contains ("damaged"));
}

TEST_CASE ("A branch another computer took its own way keeps this computer's line, with the other's as divergent")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    const auto* history = phazite::getVersionHistory (s.context);
    const auto first = history->findLastVersion()->id;
    const auto main = *history->findCurrentBranch();

    // Another computer, from the same commit, committed on main too: its own record of main says so.
    juce::String theirs;

    {
        phazite::VersionStore elsewhere (phazite::getVersionsFolder (song), "elsewhere");
        phazite::VersionRecord record;
        record.name = "Theirs";
        record.parents.add (first);
        record.branch = main.id;
        record.saved = juce::Time::currentTimeMillis();
        theirs = elsewhere.writeVersion (record, phazite::splitSong (song.getState()));
        REQUIRE (theirs.isNotEmpty());

        auto branch = main;
        branch.head = theirs;
        branch.computer = "elsewhere";
        branch.working = {};
        REQUIRE (elsewhere.writeBranch (branch));
    }

    // This computer commits its own; then the history read again, as when the project next opens.
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Mine");
    const auto mine = history->findLastVersion()->id;

    phazite::VersionHistory reread (song, {});
    REQUIRE (dispatchMessagesUntil ([&reread] { return ! reread.isReading() && ! reread.isBusy(); }));
    REQUIRE (reread.findCurrentBranch() != nullptr);
    CHECK (reread.findCurrentBranch()->head == mine);
    CHECK (reread.findCurrentBranch()->divergent == juce::StringArray (theirs));
}

TEST_CASE ("A project's last commit is read from its Versions folder without the project, to open in its place")
{
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "Kept");
    auto& song = *phazite::findSong (s.context);

    const auto [commit, name] = phazite::readLastCommitSong (song.getFile());
    REQUIRE (commit.isValid());
    CHECK (name == "Kept");
    CHECK (commit.hasType (schema::PHAZITE));
    CHECK (phazite::getTrackElements (commit).size() == phazite::getTrackElements (song.getState()).size());
}

TEST_CASE ("Commit can run only while there is something to commit, and says what it would be named while no message is typed")
{
    TestSong s;
    VersionCommands commands (s);
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);

    // Nothing since: nothing to commit, as git says, and the command waits for a change.
    CHECK_FALSE (phazite::hasSomethingToCommit (s.context));
    CHECK_FALSE (phazite::canSaveVersion (s.context));
    CHECK (phazite::describeNextCommit (s.context).isEmpty());
    CHECK_FALSE (commands.registry.invoke ("version.save"));

    // A change: a commit would be named by it.
    phazite::setChannelVolume (song, bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasSomethingToCommit (s.context); }));
    CHECK (phazite::canSaveVersion (s.context));
    CHECK (phazite::describeNextCommit (s.context) == "Update Bass");

    // Staged: what is staged names it.
    s.addTrack ("Keys");
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::getChangesSinceLastVersion (s.context).size() == 2; }));
    REQUIRE (phazite::stageChange (s.context, phazite::getChangesSinceLastVersion (s.context).back()));
    CHECK (phazite::describeNextCommit (s.context) == "Add Keys");
}

TEST_CASE ("Undo Last Commit takes the last commit off its branch, stages what it recorded again, and puts its message back")
{
    TestSong s;
    VersionCommands commands (s);
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Quieter", "The verse was too loud");
    const auto quieter = phazite::getLastVersion (s.context)->id;
    REQUIRE (phazite::getVersions (s.context).size() == 3); // and the initial commit

    // The branch back at First; what Quieter recorded staged again; the song as it is; its message the draft again.
    REQUIRE (phazite::canUndoLastCommit (s.context));
    commands.run ("version.undoCommit");
    waitForVersions (s.context);
    CHECK (phazite::getLastVersion (s.context)->name == "First");
    CHECK (phazite::getVersions (s.context).size() == 2);
    CHECK (describe (phazite::getStagedChanges (s.context)) == "Bass: volume");
    CHECK (phazite::getChangesSinceLastVersion (s.context).empty());
    CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (-6.0f));
    CHECK (phazite::getVersionDraftName (s.context) == "Quieter");
    CHECK (phazite::getVersionDraftNote (s.context) == "The verse was too loud");

    // Read again as the project next opens: the branch is at First there too.
    {
        phazite::VersionHistory reread (song, {});
        REQUIRE (dispatchMessagesUntil ([&reread] { return ! reread.isReading() && ! reread.isBusy(); }, 20000));
        REQUIRE (reread.findLastVersion() != nullptr);
        CHECK (reread.findLastVersion()->name == "First");
        CHECK (reread.findVersion (quieter) == nullptr);
    }

    // The commit is still in the store: Verify History finds it, for Recover Lost Commits.
    REQUIRE (phazite::verifyHistory (s.context));
    REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::isVerifyingHistory (s.context); }, 20000));
    CHECK (phazite::countLostVersions (s.context) == 1);

    // Committed again, with the message back in the field.
    keepVersion (s.context, phazite::getVersionDraftName (s.context));
    CHECK (phazite::getLastVersion (s.context)->name == "Quieter");
    CHECK (phazite::getStagedChanges (s.context).empty());

    // Never the first commit, which follows none.
    REQUIRE (phazite::undoLastCommit (s.context));
    REQUIRE (phazite::undoLastCommit (s.context));
    CHECK (phazite::getLastVersion (s.context)->name == "Initial commit");
    CHECK_FALSE (phazite::canUndoLastCommit (s.context));
    CHECK_FALSE (phazite::undoLastCommit (s.context));
}

TEST_CASE ("Edit Commit Message names a commit anew beside its record, which keeps its id, and the name comes back as the project opens")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context);
    const auto second = phazite::getLastVersion (s.context);
    REQUIRE (second.has_value());
    CHECK_FALSE (second->hasOwnName);
    CHECK (second->name == "Update Bass"); // named by what it changed
    CHECK (second->summary == "Update Bass");

    // Named, with a note: the same commit, its summary still there.
    phazite::selectVersion (s.context, second->id);
    CHECK (phazite::canEditSelectedMessage (s.context));
    REQUIRE (phazite::editVersionMessage (s.context, second->id, "Sent to the label", "Mix 3, quieter bass"));
    const auto edited = phazite::getLastVersion (s.context);
    CHECK (edited->id == second->id);
    CHECK (edited->name == "Sent to the label");
    CHECK (edited->note == "Mix 3, quieter bass");
    CHECK (edited->hasOwnName);
    CHECK (edited->summary == "Update Bass");

    // As the project opens again.
    {
        phazite::VersionHistory reread (song, {});
        REQUIRE (dispatchMessagesUntil ([&reread] { return ! reread.isReading() && ! reread.isBusy(); }, 20000));
        REQUIRE (reread.findVersion (second->id) != nullptr);
        CHECK (reread.findVersion (second->id)->name == "Sent to the label");
        CHECK (reread.findVersion (second->id)->note == "Mix 3, quieter bass");
    }

    // An empty name leaves it named by what it changed again.
    REQUIRE (phazite::editVersionMessage (s.context, second->id, {}, {}));
    CHECK (phazite::getLastVersion (s.context)->name == "Update Bass");
    CHECK_FALSE (phazite::getLastVersion (s.context)->hasOwnName);
    CHECK_FALSE (phazite::editVersionMessage (s.context, "not-a-commit", "Name", {}));
}

TEST_CASE ("A commit's changes are what it changed since the commit before it, and each line restores from it as one undo step")
{
    TestSong s;
    VersionCommands commands (s);
    const auto bass = s.addTrack ("Bass");
    s.addTrack ("Drums");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Quieter");
    const auto quieter = phazite::getLastVersion (s.context)->id;
    phazite::setChannelVolume (song, bass, -12.0f);
    s.addTrack ("Keys");
    keepVersion (s.context, "Keys");

    const auto changes = phazite::getVersionChanges (s.context, quieter);
    CHECK (describe (changes) == "Bass: volume");
    CHECK (describe (phazite::getVersionChanges (s.context, phazite::getLastVersion (s.context)->id)) == "Bass: volume; + Keys");
    CHECK (phazite::getVersionChanges (s.context, "not-a-commit").empty());

    // Bass as it was in Quieter, one undo step; the rest as it is.
    REQUIRE (changes.size() == 1);
    phazite::selectVersionChange (s.context, quieter, changes.front());
    CHECK (phazite::canRestoreSelectedChange (s.context));
    CHECK (phazite::describeRestoreChange (s.context) == "Restore Bass from \"Quieter\"");
    const auto tracks = phazite::getTrackIDs (song).size();
    commands.run ("version.restoreChange");
    CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (-6.0f));
    CHECK (phazite::getTrackIDs (song).size() == tracks); // Keys stays
    CHECK (phazite::describeUndo (&song) == "Undo Restore Bass from \"Quieter\"");
    phazite::undo (song);
    CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (-12.0f));

    // A commit selected alone is no line to restore.
    phazite::selectVersion (s.context, quieter);
    CHECK_FALSE (phazite::canRestoreSelectedChange (s.context));
}

TEST_CASE (
    "Local History keeps the song before Discard All Changes and Restore while it differs from its last commit, each restored as one undo step")
{
    TestSong s;
    VersionCommands commands (s);
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    CHECK (phazite::getSnapshots (s.context).empty());

    // Nothing to keep while the song is its last commit.
    REQUIRE_FALSE (phazite::discardAllChanges (s.context));
    waitForVersions (s.context);
    CHECK (phazite::getSnapshots (s.context).empty());

    // A change discarded: kept first.
    phazite::setChannelVolume (song, bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));
    REQUIRE (phazite::discardAllChanges (s.context));
    waitForVersions (s.context);
    auto snapshots = phazite::getSnapshots (s.context);
    REQUIRE (snapshots.size() == 1);
    CHECK (snapshots.front().reason == "Before Discard All Changes");
    CHECK (snapshots.front().branch == "main");
    CHECK (phazite::getChannelVolume (song, bass) != doctest::Approx (-6.0f));

    // Restored: the change back, one undo step.
    phazite::selectSnapshot (s.context, snapshots.front().time);
    REQUIRE (phazite::canRestoreSelectedSnapshot (s.context));
    CHECK (phazite::describeRestoreSnapshot (s.context).startsWith ("Restore Local History of "));
    commands.run ("version.restoreSnapshot");
    waitForVersions (s.context);
    CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (-6.0f));
    CHECK (phazite::describeUndo (&song).startsWith ("Undo Restore Local History of "));

    // Restore from a commit keeps the song as it is first.
    phazite::setChannelVolume (song, bass, -9.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));
    phazite::selectVersion (s.context, phazite::getLastVersion (s.context)->id);
    commands.run ("version.bringBack");
    waitForVersions (s.context);
    snapshots = phazite::getSnapshots (s.context);
    REQUIRE (snapshots.size() == 2);
    CHECK (snapshots.front().reason == "Before Restore"); // newest first
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    // The same song twice is kept once.
    phazite::setChannelVolume (song, bass, -3.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));
    auto* history = phazite::getVersionHistory (s.context);
    history->takeSnapshot ("auto");
    waitForVersions (s.context);
    REQUIRE (phazite::getSnapshots (s.context).size() == 3);
    history->takeSnapshot ("auto");
    waitForVersions (s.context);
    CHECK (phazite::getSnapshots (s.context).size() == 3);

    // Kept in the project's folder, as the project opens again: and taken as it opens, while it has changes since its last
    // commit.
    phazite::setChannelVolume (song, bass, -1.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::getChangesSinceLastVersion (s.context).size() == 1; }));
    phazite::VersionHistory reread (song, {});
    REQUIRE (dispatchMessagesUntil ([&reread] { return ! reread.isReading() && ! reread.isBusy(); }, 20000));
    REQUIRE (reread.getSnapshots().size() == 4);
    CHECK (reread.getSnapshots().front().reason == "open");
    CHECK (reread.readSnapshot (reread.getSnapshots().front().time).isValid());
}

TEST_CASE ("Local History thins as it grows old: every snapshot of two hours, one an hour for a day, one a day for a month")
{
    constexpr juce::int64 minute = 60 * 1000, hour = 60 * minute, day = 24 * hour;
    constexpr juce::int64 now = 1000 * day;
    std::vector<phazite::Snapshot> all;

    // Every ten minutes for 40 days, oldest first.
    for (auto time = now - 40 * day; time <= now; time += 10 * minute)
        all.push_back ({ time, "main", "song", "auto" });

    const auto kept = phazite::thinSnapshots (all, now);
    const auto count = [&kept] (juce::int64 from, juce::int64 to)
    { return std::count_if (kept.begin(), kept.end(), [from, to] (const auto& s) { return now - s.time >= from && now - s.time < to; }); };

    CHECK (count (0, 2 * hour + 1) == 13);       // every one of the last two hours
    CHECK (count (2 * hour + 1, day + 1) == 22); // one an hour, to a day
    CHECK (count (day + 1, 30 * day + 1) == 29); // one a day, to 30 days
    CHECK (count (30 * day + 1, 41 * day) == 0); // none older
    CHECK (std::is_sorted (kept.begin(), kept.end(), [] (const auto& a, const auto& b) { return a.time < b.time; }));
    CHECK (kept.back().time == now);
}
