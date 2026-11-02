// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GoldenFiles.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"

using namespace tracktion::literals;

namespace
{
// A song's first track playing a two-bar MIDI clip on Phazite's synth, so renders and freezes produce audio.
void addSynthAndNotes (te::Edit& edit)
{
    auto* track = te::getAudioTracks (edit)[0];
    phazite::insertDevice (*track, edit.getPluginCache().createNewPlugin (phazite::devices::Synth::typeID, {}));
    auto* clip = phazite::test::addMidiClip (*track, { 0_bp, 8_bp }, "Notes");

    for (int i = 0; i < 8; ++i)
        phazite::test::addMidiNote (*clip, 60 + i, te::BeatPosition::fromBeats (i), te::BeatDuration::fromBeats (0.5), 100);
}

std::unique_ptr<te::Edit> createEditWithSynth (te::Engine& engine, const juce::File& file)
{
    auto edit = phazite::createSongEdit (engine, file);
    addSynthAndNotes (*edit);
    return edit;
}
} // namespace

TEST_CASE ("Saved projects contain no personal data")
{
    const phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Song", error, addSynthAndNotes);
    REQUIRE (project != nullptr);
    REQUIRE (project->save().wasOk());
    const auto xml = phazite::test::readXml (project->getFile());
    REQUIRE (xml != nullptr);

    // No value holds the user's full name or login name (part of their home folder's path).
    phazite::test::checkNoUserName ("The save", *xml);

    // Short names that every save holds in its tags and attribute names (TIMESIGNATURE, start, length) don't count: a user
    // called Tim, Art, Len or Ann, or logged in as dev, isn't reported. This reads the save as the golden files do
    // (normaliseProject()), without what is the machine's.
    const auto withoutInputs = phazite::test::normaliseProject (*xml, project->getFolder());
    CHECK (phazite::test::findValuesNaming (*withoutInputs, { "tim", "dev", "art", "len", "ann" }).joinIntoString ("\n") == juce::String());
}

TEST_CASE ("Rendering and freezing run their tasks without a modal loop")
{
    auto& engine = phazite::test::getEngine();
    juce::TemporaryFile editFile (".phazite");
    auto edit = createEditWithSynth (engine, editFile.getFile());

    SUBCASE ("Render the project to a WAV file")
    {
        juce::TemporaryFile wav (".wav");
        REQUIRE (te::Renderer::renderToFile (*edit, wav.getFile(), false));
        CHECK (wav.getFile().getSize() > 10000);
    }

    SUBCASE ("Freeze and unfreeze a track")
    {
        auto* track = te::getAudioTracks (*edit)[0];
        REQUIRE (track->setFrozen (true, te::Track::individualFreeze).wasOk());
        CHECK (track->isFrozen (te::Track::individualFreeze));

        REQUIRE (track->setFrozen (false, te::Track::individualFreeze).wasOk());
        CHECK_FALSE (track->isFrozen (te::Track::anyFreeze));
    }
}

TEST_CASE ("The focused Edit is forgotten when it is deleted")
{
    auto& engine = phazite::test::getEngine();
    auto& ui = phazite::getUIBehaviour (engine);
    auto edit = phazite::createSongEdit (engine);

    ui.setFocusedEdit (edit.get());
    CHECK (ui.getLastFocusedEdit() == edit.get());
    CHECK (ui.getCurrentlyFocusedEdit() == edit.get());
    CHECK (ui.getAllOpenEdits().size() == 1);

    edit = nullptr;
    CHECK (ui.getLastFocusedEdit() == nullptr);
    CHECK (ui.getAllOpenEdits().isEmpty());
}

TEST_CASE ("Recordings go into the project's Audio folder")
{
    auto& engine = phazite::test::getEngine();
    juce::TemporaryFile file (".phazite");
    auto edit = phazite::createSongEdit (engine, file.getFile());

    CHECK (engine.getEngineBehaviour().getDefaultFolderForAudioRecordings (*edit) == file.getFile().getSiblingFile ("Audio"));
}

TEST_CASE ("Edit limits leave room for one-shot-heavy projects")
{
    phazite::EngineBehaviour behaviour ({ .openAudioDevice = false });
    const auto limits = behaviour.getEditLimits();

    CHECK (limits.maxPluginsOnTrack >= 64);
    CHECK (limits.maxClipsInTrack >= 5000);
    CHECK (limits.maxNumTracks >= 1000);
}
