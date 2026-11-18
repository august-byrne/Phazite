// SPDX-License-Identifier: AGPL-3.0-or-later
// A project's versions (docs/design/versions.md): saving them, reading them back as the project opens, and the changes since the
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
#include "model/versions/internal/VersionStore.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/DevicesView.h"

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

// Saves a version with a name and a note, and waits until it is written.
void keepVersion (phazite::ProjectContext& context, const juce::String& name = {}, const juce::String& note = {})
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, note);
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    INFO (result->getErrorMessage().toStdString());
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
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
    return describe (phazite::getChangesSinceLastVersion (withChangesIn (context)));
}

// The versions' commands as the app registers them (model/versions/VersionsModel.h), for a test's song, whose saves always
// succeed; each run as a menu runs it, which must be enabled.
struct VersionCommands
{
    explicit VersionCommands (TestSong& s)
        : context (s.context)
    {
        phazite::addVersionCommands (registry, s.context, [] (std::function<void (bool)> then) { then (true); }, s.renders);
    }

    // Run as a menu runs it, and done once what it read, if it reads, is taken in (finished()).
    void run (const juce::String& id)
    {
        REQUIRE (phazite::test::finished (context, [&] { return registry.invoke (id); }));
    }

    phazite::ProjectContext& context;
    phazite::CommandRegistry registry;
};
} // namespace

TEST_CASE ("A project starts with its initial commit on main, and lists each commit after it")
{
    TestSong s;
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::keepsVersions (s.context));
    const auto initial = phazite::getVersions (s.context);
    REQUIRE (initial.size() == 1);
    CHECK (initial[0].name == "First version");
    CHECK (initial[0].parents.isEmpty());
    CHECK (phazite::getCurrentBranch (s.context)->name == "main");
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (s.getProject().getFolder().getChildFile ("Versions").isDirectory());

    s.addTrack ("Drums"); // a version records a change: one of none isn't written
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
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    // An undo back to where the version was is no change either.
    s.setVolume (track, -3.0f);
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    phazite::undo (song);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK (values().getNumProperties() == 1);

    // Its mix too; and the value set before the version set back, which leaves the device no values at all, as it was made.
    phazite::setMix (song, compressor, 0.5f);
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    phazite::setMix (song, compressor, 1.0f);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    s.setParameter (compressor, "attack", defaultOf (compressorParameters, "attack"));
    CHECK_FALSE (values().isValid());
    CHECK (describeChanges (s.context) == "Keys: Compressor");
    phazite::undo (song);
    CHECK ((float) values()["attack"] == doctest::Approx (30.0f));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    // A modulator's parameter: an LFO's rate, and back; and an envelope follower's depth, which it starts at, its default.
    s.setModulatorParameter (lfo, "rate", 5.0f);
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    s.setModulatorParameter (lfo, "rate",
                             defaultOf (phazite::devices::getModulatorParameters (phazite::devices::ModulatorKind::lfo), "rate"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    s.setModulatorParameter (follower, "depth", 0.5f);
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    s.setModulatorParameter (
        follower, "depth",
        defaultOf (phazite::devices::getModulatorParameters (phazite::devices::ModulatorKind::envelopeFollower), "depth"));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    // The faders too, the master's to where it starts, a little down, which its double-click sets (it set 0 dB: 2026-11-09).
    const auto setBack = [&s] (phazite::ParameterHandle fader)
    {
        INFO (fader.getName().toStdString());
        REQUIRE (fader.getDefaultValue().has_value());
        fader.setValue (0.3f);
        CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
        fader.setValue (*fader.getDefaultValue());
        CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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

    // A moment later, as a person commits: the plug-in, opened from no state of the song's, has given its own (a commit
    // writes what each gave last: model/plugins/internal/PluginInstances.h).
    REQUIRE (dispatchMessagesUntil (
        [&s]
        {
            const auto& opened = *phazite::findSong (s.context);
            return phazite::getPluginsWithOlderStates (opened).isEmpty() && phazite::getPluginsAnswering (opened).isEmpty();
        }));
    keepVersion (s.context);
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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
            CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
            mouse.doubleClick (centre);
            dispatchPendingMessages();
            CHECK (knob()->getValue() == doctest::Approx (knob()->getDoubleClickReturnValue()));
            CHECK (describeChanges (s.context) == "");

            turn (*knob());
            CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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

TEST_CASE ("A plug-in still loading as a version is kept shows a knob turned after it has loaded as a change since that version")
{
    // The review of known bug 166: a commit made while a plug-in loaded (in the background, as one added does) kept no
    // values of its knobs, and one turned in Phazite after it had loaded showed as no change.
    TestPlugin testPlugin;
    TestSong s;
    auto& song = *phazite::findSong (s.context);

    // Added from settings whose Gain is a quarter, and kept as a version while it still loads.
    juce::MemoryOutputStream state;
    state.writeFloat (0.25f); // its Gain
    state.writeFloat (0.5f);  // its Tone
    auto device = phazite::makeDeviceState (song, phazite::makeDeviceType (TestPlugin::getDescription()));
    device.setProperty (schema::pluginState, phazite::writeBytes (state.getMemoryBlock()), nullptr);
    phazite::DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Add the stand-in");
        plugin = phazite::insertDeviceState (song, s.firstTrack, device);
    }

    phazite::findPluginInstance (song, plugin); // its load begun
    REQUIRE (phazite::getPluginStatus (song, plugin) == phazite::PluginInstances::Status::loading);
    keepVersion (s.context);
    s.waitForPlugins();
    CHECK (describeChanges (s.context) == "");

    // Its Gain set to its default by a control after it has loaded, which leaves no value in the song: a change all the same.
    s.setParameter (plugin, TestPlugin::gainID, 1.0f);
    CHECK (describeChanges (s.context).contains ("Gain 25%"));
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
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    // So do a solo, alone or not, and a clip's mute.
    phazite::toggleTracksSoloed (song, { track });
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    phazite::toggleTracksSoloed (song, { track });
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    phazite::toggleSoloAlone (song, other);
    phazite::toggleSoloAlone (song, other);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    phazite::toggleClipsMuted (song, { clip });
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    phazite::toggleClipsMuted (song, { clip });
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
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
    REQUIRE (waitForVersionHistory (s.context));

    const auto versions = phazite::getVersions (s.context);
    REQUIRE (versions.size() == 3);
    CHECK (versions[0].name == "Two");
    CHECK (versions[1].name == "One");
    CHECK (versions[2].name == "First version");
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

    REQUIRE (waitForVersionHistory (s.context));
    s.addTrack ("Lead");
    REQUIRE (s.getProject().hasUnsavedChanges());
    REQUIRE (commands.invoke ("version.save"));
    REQUIRE (waitForVersionHistory (s.context));

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
    REQUIRE (waitForVersionHistory (context));

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

TEST_CASE ("Saving a version records the whole song as it is, and Discard Changes makes a line what the last version has")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    keepVersion (s.context, "Start");
    const auto volume = [&s] (phazite::TrackID track) { return phazite::getChannelVolume (*phazite::findSong (s.context), track); };

    s.setVolume (bass, -6.0f);
    s.setVolume (drums, -9.0f);
    CHECK (describeChanges (s.context) == "Bass: volume; Drums: volume");
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 2);

    // Discard Changes on Bass: what the last version has, as one undo step; Drums stays changed.
    REQUIRE (phazite::takeBack (s.context, phazite::getChangesSinceLastVersion (withChangesIn (s.context))[0]));
    CHECK (volume (bass) == doctest::Approx (0.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard Changes in Bass");
    CHECK (describeChanges (s.context) == "Drums: volume");
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 1);

    // A version records every change, the whole song as it is: no change is left since it.
    s.setVolume (bass, -3.0f);
    auto* history = phazite::getVersionHistory (s.context);
    const auto start = phazite::getLastVersion (s.context)->id;
    keepVersion (s.context, "Both down");
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (phazite::countChanges (withChangesIn (s.context)) == 0);
    const auto saved = history->readSong (phazite::getLastVersion (s.context)->id);
    const auto recorded = phazite::findChanges (history->readSong (start), saved);
    REQUIRE (recorded.size() == 2);
    CHECK (recorded[0].name == "Bass");
    CHECK (recorded[1].name == "Drums");
    CHECK (phazite::findChanges (saved, phazite::findSong (s.context)->getState()).empty());

    // Discard All Changes: the song as the last version has it again, one undo step.
    s.setVolume (bass, 0.0f);
    s.addTrack ("Keys");
    REQUIRE (phazite::discardAllChanges (s.context));
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (volume (bass) == doctest::Approx (-3.0f));
    CHECK (phazite::describeUndo (phazite::findSong (s.context)) == "Undo Discard All Changes");
}

TEST_CASE ("A branch is named as typed, spaces and all, each run of white space one space, without control characters")
{
    CHECK (phazite::toBranchName ("Half time") == "Half time");
    CHECK (phazite::toBranchName ("  big   chorus ") == "big chorus");
    CHECK (phazite::toBranchName ("what?*[now]:~^") == "what?*[now]:~^");
    CHECK (phazite::toBranchName ("Verse\tidea\n2") == "Verse idea 2");
    CHECK (phazite::toBranchName (juce::String ("Bridge") + juce::String::charToString (7) + "!") == "Bridge!");
    CHECK (phazite::toBranchName ("   ") == "");

    // Format characters, which change how text shows without showing themselves, go; a name stops at 100 characters.
    CHECK (phazite::toBranchName (juce::CharPointer_UTF8 ("Ver\xe2\x80\x8bse \xe2\x80\xaeidea\xe2\x80\xac")) == "Verse idea");
    CHECK (phazite::toBranchName (juce::String::repeatedString ("ab", 80)).length() == phazite::maxBranchName);
    CHECK (phazite::toBranchName (juce::String::repeatedString ("a", 99) + "  bc") == juce::String::repeatedString ("a", 99));
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

TEST_CASE ("Verify History finds the versions no branch holds and what is damaged, and Recover Lost Versions makes a branch of each line")
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
        REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::isVerifyingHistory (s.context); }));
    };

    // No log names it: only the check finds it, and Recover Lost Versions lists it as found by the check.
    CHECK (phazite::getLostVersions (s.context).empty());
    verify();
    CHECK (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    const auto lost = phazite::getLostVersions (s.context);
    REQUIRE (lost.size() == 1);
    CHECK (lost[0].why == "found");
    CHECK (lost[0].lastName == "Lost");
    CHECK (phazite::describeHistoryCheck (s.context).contains ("Recover Lost Versions"));

    // Recovered: a branch at it, named after the branch it was made on.
    CHECK (phazite::recoverLostVersions (s.context, lost[0].key).isNotEmpty());
    CHECK (phazite::getLostVersions (s.context).empty());
    const auto branches = phazite::getBranches (s.context);
    CHECK (std::any_of (branches.begin(), branches.end(), [&lostID] (const phazite::BranchInfo& branch)
                        { return branch.name == "main (recovered)" && branch.head == lostID; }));

    // A byte of a pack damaged, found by what the pack holds (not by its file's time, which some systems keep in whole
    // seconds): the pack that lists the new version's record, which no other pack holds.
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Second");
    const auto second = phazite::getVersionHistory (s.context)->findLastVersion()->id;
    juce::File holding;
    juce::MemoryBlock bytes;
    size_t pieceStart = 0, pieceSize = 0, indexStart = 0;

    for (const auto& pack :
         phazite::getVersionsFolder (song).getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack"))
    {
        juce::MemoryBlock read;
        REQUIRE (pack.loadFileAsData (read));
        REQUIRE (read.getSize() > 32);
        const auto* end = static_cast<const char*> (read.getData()) + read.getSize() - 32;
        const auto count = (size_t) juce::ByteOrder::littleEndianInt (end);
        const auto start = (size_t) juce::ByteOrder::littleEndianInt64 (end + 4);

        for (size_t i = 0; i < count; ++i)
        {
            const auto* entry = static_cast<const char*> (read.getData()) + start + i * 56;

            if (juce::String::toHexString (entry, 32, 0) == second)
            {
                holding = pack;
                bytes = read;
                pieceStart = (size_t) juce::ByteOrder::littleEndianInt64 (entry + 32);
                pieceSize = juce::ByteOrder::littleEndianInt (entry + 40);
                indexStart = start;
            }
        }
    }

    REQUIRE (holding.existsAsFile());
    REQUIRE (pieceSize > 2);

    SUBCASE ("A byte of a piece: the piece is found damaged")
    {
        static_cast<char*> (bytes.getData())[pieceStart + pieceSize / 2] ^= 0x5a;
        REQUIRE (holding.replaceWithData (bytes.getData(), bytes.getSize()));
        verify();
        CHECK (phazite::describeHistoryCheck (s.context).contains ("1 piece is damaged"));
        CHECK_FALSE (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    }

    SUBCASE ("A byte of the pack's index: the pack can't be read")
    {
        static_cast<char*> (bytes.getData())[indexStart + 40] ^= 0x5a;
        REQUIRE (holding.replaceWithData (bytes.getData(), bytes.getSize()));
        verify();
        CHECK (phazite::describeHistoryCheck (s.context).contains ("1 pack of the Versions folder can't be read"));
        CHECK_FALSE (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    }

    SUBCASE ("A byte of the pack's trailer: the pack can't be read")
    {
        static_cast<char*> (bytes.getData())[bytes.getSize() - 10] ^= 0x5a;
        REQUIRE (holding.replaceWithData (bytes.getData(), bytes.getSize()));
        verify();
        CHECK (phazite::describeHistoryCheck (s.context).contains ("1 pack of the Versions folder can't be read"));
        CHECK_FALSE (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    }
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
    REQUIRE (dispatchMessagesUntil (
        [&reread]
        {
            reread.getChanges(); // its snapshot as it opened, once its changes are worked out
            return ! reread.isReading() && ! reread.isBusy() && reread.areChangesWorkedOut();
        },
        versionHistoryTimeout));
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

    const auto [commit, name] = phazite::readLastCommitSong (song.getFile(), phazite::getThisComputer());
    REQUIRE (commit.isValid());
    CHECK (name == "Kept");
    CHECK (commit.hasType (schema::PHAZITE));
    CHECK (phazite::getTrackElements (commit).size() == phazite::getTrackElements (song.getState()).size());
}

TEST_CASE ("Save Version can run only while there is something to save, and says what it would be named while no name is typed")
{
    TestSong s;
    VersionCommands commands (s);
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);

    // Nothing since: nothing to save, and the command waits for a change.
    CHECK_FALSE (phazite::hasSomethingToCommit (withChangesIn (s.context)));
    CHECK_FALSE (phazite::canSaveVersion (withChangesIn (s.context)));
    CHECK (phazite::describeNextCommit (withChangesIn (s.context)).isEmpty());
    CHECK_FALSE (commands.registry.invoke ("version.save"));

    // A change: a version would be named by it; another, by both.
    phazite::setChannelVolume (song, bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasSomethingToCommit (withChangesIn (s.context)); }));
    CHECK (phazite::canSaveVersion (withChangesIn (s.context)));
    CHECK (phazite::describeNextCommit (withChangesIn (s.context)) == "Update Bass");
    s.addTrack ("Keys");
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::getChangesSinceLastVersion (withChangesIn (s.context)).size() == 2; }));
    CHECK (phazite::describeNextCommit (withChangesIn (s.context)) == "Add Keys; update Bass");
}

TEST_CASE ("Undo Last Version takes the last version off its branch, leaves the song, shows what it recorded again, and puts its name back")
{
    TestSong s;
    VersionCommands commands (s);
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Quieter", "The verse was too loud");
    const auto quieter = phazite::getLastVersion (s.context)->id;
    REQUIRE (phazite::getVersions (s.context).size() == 3); // and the first version
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    // The branch back at First; the song as it is; what Quieter recorded in the changes again; its name and note the draft
    // again.
    REQUIRE (phazite::canUndoLastCommit (s.context));
    const auto undoBefore = phazite::describeUndo (&song);
    commands.run ("version.undoLast");
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getLastVersion (s.context)->name == "First");
    CHECK (phazite::getVersions (s.context).size() == 2);
    CHECK (describeChanges (s.context) == "Bass: volume");
    CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (-6.0f));
    CHECK (phazite::getVersionDraftName (s.context) == "Quieter");
    CHECK (phazite::getVersionDraftNote (s.context) == "The verse was too loud");
    CHECK (phazite::describeUndo (&song) == undoBefore); // not an undo step: the song didn't change

    // Read again as the project next opens: the branch is at First there too.
    {
        phazite::VersionHistory reread (song, {});
        REQUIRE (dispatchMessagesUntil ([&reread] { return ! reread.isReading() && ! reread.isBusy(); }, versionHistoryTimeout));
        REQUIRE (reread.findLastVersion() != nullptr);
        CHECK (reread.findLastVersion()->name == "First");
        CHECK (reread.findVersion (quieter) == nullptr);
    }

    // The version is still in the store, and Recover Lost Versions lists it as undone.
    const auto lost = phazite::getLostVersions (s.context);
    REQUIRE (lost.size() == 1);
    CHECK (lost[0].why == "undo");
    CHECK (lost[0].lastName == "Quieter");
    CHECK (lost[0].numVersions == 1);

    // Discard Changes after it: the line what First has, one undo step.
    {
        const auto line = phazite::getChangesSinceLastVersion (withChangesIn (s.context));
        REQUIRE (line.size() == 1);
        REQUIRE (phazite::takeBack (s.context, line[0]));
        CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (0.0f));
        CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
        phazite::undo (song);
        CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (-6.0f));
        CHECK (describeChanges (s.context) == "Bass: volume");
    }

    // Saved again, with the name back in the field.
    keepVersion (s.context, phazite::getVersionDraftName (s.context));
    CHECK (phazite::getLastVersion (s.context)->name == "Quieter");
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    // Never the first version, which follows none.
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
    CHECK (phazite::getLastVersion (s.context)->name == "First version");
    CHECK_FALSE (phazite::canUndoLastCommit (s.context));
    CHECK_FALSE (phazite::test::finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
}

TEST_CASE ("Rename Version names a version anew beside its record, which keeps its id, and the name comes back as the project opens")
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
        REQUIRE (dispatchMessagesUntil ([&reread] { return ! reread.isReading() && ! reread.isBusy(); }, versionHistoryTimeout));
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

    // Read on the history's thread, as a commit is opened: none listed meanwhile, then what it changed (known bug 126).
    const auto songsRead = phazite::VersionStore::countSongsRead();
    CHECK (phazite::getVersionChanges (s.context, quieter).empty());
    CHECK (phazite::isReadingVersionChanges (s.context, quieter));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK_FALSE (phazite::isReadingVersionChanges (s.context, quieter));
    const auto changes = phazite::getVersionChanges (s.context, quieter);
    CHECK (describe (changes) == "Bass: volume");
    CHECK (phazite::VersionStore::countSongsRead().onMessageThread == songsRead.onMessageThread);
    phazite::getVersionChanges (s.context, phazite::getLastVersion (s.context)->id);
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (describe (phazite::getVersionChanges (s.context, phazite::getLastVersion (s.context)->id)) == "Bass: volume; + Keys");
    CHECK (phazite::getVersionChanges (s.context, "not-a-commit").empty());
    CHECK_FALSE (phazite::isReadingVersionChanges (s.context, "not-a-commit"));

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
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getSnapshots (s.context).empty());

    // A change discarded: kept first.
    phazite::setChannelVolume (song, bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
    REQUIRE (phazite::discardAllChanges (s.context));
    REQUIRE (waitForVersionHistory (s.context));
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
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getChannelVolume (song, bass) == doctest::Approx (-6.0f));
    CHECK (phazite::describeUndo (&song).startsWith ("Undo Restore Local History of "));

    // Restore from a commit keeps the song as it is first.
    phazite::setChannelVolume (song, bass, -9.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
    phazite::selectVersion (s.context, phazite::getLastVersion (s.context)->id);
    commands.run ("version.bringBack");
    REQUIRE (waitForVersionHistory (s.context));
    snapshots = phazite::getSnapshots (s.context);
    REQUIRE (snapshots.size() == 2);
    CHECK (snapshots.front().reason == "Before Restore"); // newest first
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    // The same song twice is kept once.
    phazite::setChannelVolume (song, bass, -3.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
    auto* history = phazite::getVersionHistory (s.context);
    history->takeSnapshot ("auto");
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (phazite::getSnapshots (s.context).size() == 3);
    history->takeSnapshot ("auto");
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getSnapshots (s.context).size() == 3);

    // Kept in the project's folder, as the project opens again: and taken as it opens, while it has changes since its last
    // commit.
    phazite::setChannelVolume (song, bass, -1.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::getChangesSinceLastVersion (withChangesIn (s.context)).size() == 1; }));
    phazite::VersionHistory reread (song, {});
    REQUIRE (dispatchMessagesUntil (
        [&reread]
        {
            reread.getChanges(); // its snapshot as it opened, once its changes are worked out
            return ! reread.isReading() && ! reread.isBusy() && reread.areChangesWorkedOut();
        },
        versionHistoryTimeout));
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

TEST_CASE ("A song another tool wrote with switches as true and false and IDs as 12.0 opens as its last commit: no changes, no snapshot")
{
    // Known bug 179: such a file plays as Phazite's own does, but the versions compare the text, so a file equal to its last
    // commit showed changes, and took an "As opened" snapshot.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 60, 0.0, 1.0);
    auto& song = *phazite::findSong (s.context);
    phazite::setTrackMuted (song, bass, true);
    phazite::setCollapsed (song, bass, false); // a switch Phazite writes as 0
    keepVersion (s.context, "First");
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    // The same song, each switch and ID as another tool writes it.
    auto written = song.getState().createCopy();
    const std::function<void (juce::ValueTree)> rewrite = [&rewrite] (juce::ValueTree element)
    {
        for (auto i = element.getNumProperties(); --i >= 0;)
        {
            const auto name = element.getPropertyName (i);
            const auto text = element[name].toString();

            if (element.hasType (schema::TRACK) && name == schema::trackMute)
                element.setProperty (name, "true", nullptr);
            else if (element.hasType (schema::TRACK) && name == schema::trackCollapsed)
                element.setProperty (name, "false", nullptr);
            else if ((schema::holdsID (element, name) || (element.hasType (schema::NOTE) && name == schema::noteID))
                     && text.containsOnly ("0123456789"))
                element.setProperty (name, text + ".0", nullptr);
        }

        for (auto child : element)
            rewrite (child);
    };

    rewrite (written);
    written.getChildWithName (schema::TRACKS).getChild (0).setProperty (schema::trackSolo, "false", nullptr); // off, as none is

    // Written into the project's file as Phazite writes the rest (each number in its one form), and opened from it.
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, written);
    REQUIRE (s.getProject().getFile().replaceWithData (text.getData(), text.getDataSize()));
    REQUIRE (s.getProject().getFile().loadFileAsString().contains ("mute=\"true\""));
    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();
    REQUIRE (waitForVersionHistory (s.context));

    INFO (describeChanges (s.context).toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (phazite::getSnapshots (s.context).empty());
    const auto bassState = phazite::findTrackState (*phazite::findSong (s.context), bass);
    REQUIRE (bassState.isValid());
    CHECK (bassState[schema::trackMute].toString() == "1");
    CHECK (bassState[schema::trackID].toString() == juce::String ((juce::int64) bass.getRaw()));

    // What Phazite wrote opens as it was: saved again, byte for byte the same file.
    REQUIRE (s.getProject().save().wasOk());
    const auto once = s.getProject().getFile().loadFileAsString();
    s.reopen();
    REQUIRE (s.getProject().save().wasOk());
    CHECK (s.getProject().getFile().loadFileAsString() == once);
}
