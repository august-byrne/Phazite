// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/EngineSetup.h"

using namespace tracktion::literals;

namespace
{
// An Edit with one 4OSC track playing a two-bar MIDI clip, so renders and freezes produce audio.
std::unique_ptr<te::Edit> createEditWithSynth (te::Engine& engine, const juce::File& file)
{
    auto edit = te::createEmptyEdit (engine, file);
    auto* track = te::getAudioTracks (*edit)[0];

    auto synth = edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {});
    track->pluginList.insertPlugin (synth, 0, nullptr);

    auto clip = track->insertMIDIClip ("Notes", edit->tempoSequence.toTime ({ 0_bp, 8_bp }), nullptr);

    for (int i = 0; i < 8; ++i)
        clip->getSequence().addNote (60 + i, te::BeatPosition::fromBeats (i), te::BeatDuration::fromBeats (0.5), 100, 0, nullptr);

    return edit;
}

void saveEdit (te::Edit& edit)
{
    te::EditFileOperations (edit).save (false, true, false); // no warning dialog, save even if unchanged
}
} // namespace

TEST_CASE ("Saved projects contain no personal data")
{
    auto& engine = phazite::test::getEngine();
    juce::TemporaryFile file (".tracktionedit");

    {
        auto edit = createEditWithSynth (engine, file.getFile());
        saveEdit (*edit);
    }

    const auto text = file.getFile().loadFileAsString();
    const auto xml = juce::parseXML (text);
    REQUIRE (xml != nullptr);

    // Tracktion writes these two attributes on every save.
    CHECK (xml->hasAttribute ("modifiedBy"));
    CHECK (xml->getStringAttribute ("modifiedBy").isEmpty());
    CHECK (xml->getStringAttribute ("appVersion") == phazite::getVersionString());

    // Neither the user's full name nor their login name (part of their home folder's path) appears.
    for (const auto& name : { juce::SystemStats::getFullUserName(), juce::SystemStats::getLogonName() })
        if (name.length() >= 3)
            CHECK_FALSE (text.containsIgnoreCase (name));
}

TEST_CASE ("Rendering and freezing run their tasks without a modal loop")
{
    auto& engine = phazite::test::getEngine();
    juce::TemporaryFile editFile (".tracktionedit");
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
    auto edit = te::Edit::createSingleTrackEdit (engine);

    ui.setFocusedEdit (edit.get());
    CHECK (ui.getLastFocusedEdit() == edit.get());
    CHECK (ui.getCurrentlyFocusedEdit() == edit.get());
    CHECK (ui.getAllOpenEdits().size() == 1);

    edit = nullptr;
    CHECK (ui.getLastFocusedEdit() == nullptr);
    CHECK (ui.getAllOpenEdits().isEmpty());
}

TEST_CASE ("loadEdit restores a saved project on the message thread")
{
    auto& engine = phazite::test::getEngine();
    juce::TemporaryFile file (".tracktionedit");

    {
        auto edit = createEditWithSynth (engine, file.getFile());
        saveEdit (*edit);
    }

    auto loaded = phazite::loadEdit (engine, file.getFile());
    REQUIRE (loaded != nullptr);

    const auto tracks = te::getAudioTracks (*loaded);
    REQUIRE (tracks.size() == 1);
    CHECK (tracks[0]->getClips().size() == 1);

    CHECK (phazite::loadEdit (engine, file.getFile().getSiblingFile ("does not exist.tracktionedit")) == nullptr);
}

TEST_CASE ("Recordings go into the project's Audio folder")
{
    auto& engine = phazite::test::getEngine();
    juce::TemporaryFile file (".tracktionedit");
    auto edit = te::createEmptyEdit (engine, file.getFile());

    CHECK (engine.getEngineBehaviour().getDefaultFolderForAudioRecordings (*edit) == file.getFile().getSiblingFile ("Audio"));
}

TEST_CASE ("Edit limits leave room for one-shot-heavy projects")
{
    phazite::EngineBehaviour behaviour ({ .openAudioDevice = false, .scanPluginsOutOfProcess = false });
    const auto limits = behaviour.getEditLimits();

    CHECK (limits.maxPluginsOnTrack >= 64);
    CHECK (limits.maxClipsInTrack >= 5000);
    CHECK (limits.maxNumTracks >= 1000);
}
