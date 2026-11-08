// SPDX-License-Identifier: AGPL-3.0-or-later
// The sampler's editor (src/ui/devices/SamplerEditor.h), driven as the user would: its sound named and drawn, a control for
// every parameter, the sound menu, a sound dropped on the drawing, its markers dragged as one undo step each (snapping to a
// zero crossing), the controls of the mode it plays in, slices added under the drawing and taken away by a double-click,
// and how it fits the default dock.
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/Sampler.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/SamplerEditor.h"

namespace
{
using phazite::devices::Sampler;
using phazite::ui::SamplerEditor;

// The sampler on a project's first track, and its editor in a window.
struct Screen
{
    Screen()
    {
        sampler = song.setInstrument (song.firstTrack, Sampler::typeID);
        REQUIRE (sampler.isValid());
        phazite::getSession (song.context)->update();
        getSong().getUndoManager().clearUndoHistory();
        editor = std::make_unique<SamplerEditor> (phazite::makeDeviceHandle (getSong(), sampler));
        window = std::make_unique<phazite::test::TestWindow> (*editor, editor->getWidth(), editor->getHeight());
    }

    ~Screen()
    {
        window = nullptr;
        editor = nullptr;
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }
    float getValue (const char* id) const { return phazite::getValueSetByHand (getSong(), phazite::ParameterRef { sampler, id }); }
    juce::String describeUndo() const { return phazite::describeUndo (&getSong()); }

    void settle()
    {
        phazite::getSession (song.context)->update();
        editor->refresh();
    }

    phazite::test::TemporaryFolder temp;
    phazite::test::TestSong song;
    phazite::DeviceID sampler;
    std::unique_ptr<SamplerEditor> editor;
    std::unique_ptr<phazite::test::TestWindow> window;
};
} // namespace

TEST_CASE ("The sampler's editor names its sound and has a titled control for every parameter")
{
    Screen screen;
    auto& editor = *screen.editor;
    CHECK (
        dynamic_cast<SamplerEditor*> (phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (screen.getSong(), screen.sampler)).get())
        != nullptr);
    CHECK (editor.getSoundButton().getButtonText() == juce::String ("808 Bass"));
    CHECK (editor.getSampleView().getTitle() == juce::String ("Sound"));

    for (const auto& parameter : Sampler::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        const auto* control = editor.findControl (parameter.id);
        REQUIRE (control != nullptr);
        CHECK (control->getTitle() == juce::String (parameter.name.data(), parameter.name.size()));
    }
}

TEST_CASE ("The sampler's sound menu lists its factory sounds by kit, and puts the one chosen on it")
{
    Screen screen;
    auto& editor = *screen.editor;
    auto menu = editor.getSoundMenu();
    const auto* bass = phazite::test::findMenuItem (menu, { "808 Bass" });
    REQUIRE (bass != nullptr);
    CHECK (bass->isTicked);
    CHECK (phazite::test::findMenuItem (menu, { "Load Sound File..." }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Find Root Key" }) != nullptr);
    REQUIRE (phazite::test::findMenuItem (menu, { "808 Kit", "808 Kick" }) != nullptr);

    REQUIRE (phazite::test::runMenuItem (menu, { "808 Kit", "808 Kick" }));
    screen.settle();
    CHECK (editor.getSoundButton().getButtonText() == juce::String ("808 Kick"));
    CHECK (screen.describeUndo() == juce::String ("Undo Put 808 Kick on the Sampler"));
}

TEST_CASE ("A sound dropped on the sampler's drawing is put on it, as one undo step")
{
    Screen screen;
    auto& editor = *screen.editor;
    const auto sound = phazite::test::writeSound (screen.temp.folder.getChildFile ("Drops/Vox.wav"), 0.5);
    REQUIRE (editor.dropFile (sound));
    screen.settle();
    CHECK (editor.getSoundButton().getButtonText() == juce::String ("Vox"));
    CHECK (screen.describeUndo() == juce::String ("Undo Put Vox on the Sampler"));
    CHECK (screen.getValue ("root") == 57.0f); // the sine at 220 Hz: A3

    const auto text = screen.temp.folder.getChildFile ("words.txt");
    REQUIRE (text.replaceWithText ("words"));
    CHECK_FALSE (editor.dropFile (text));
}

TEST_CASE ("Dragging the sampler's start marker moves it as one undo step, onto a zero crossing")
{
    Screen screen;
    auto& editor = *screen.editor;
    auto& view = editor.getSampleView();
    const auto startX = editor.getMarkerX (Sampler::start);
    phazite::test::Mouse mouse (view);
    const auto from = juce::Point<float> (startX, (float) view.getHeight() / 2.0f).toInt();

    mouse.press (from);
    mouse.dragTo (from.translated (view.getWidth() / 4, 0));
    mouse.release();
    const auto start = screen.getValue ("start");
    CHECK (start > 0.2f);
    CHECK (start < 0.3f);
    CHECK (screen.describeUndo() == juce::String ("Undo Change Start"));

    // On a zero crossing of the 808.
    const auto sound = phazite::devices::getFactorySample (0);
    const auto at = (int) std::lround (start * (float) sound->audio.getNumSamples());
    CHECK (std::abs (sound->audio.getSample (0, at)) < 0.02f);

    phazite::undo (screen.getSong());
    CHECK (screen.getValue ("start") == 0.0f);
}

TEST_CASE ("The sampler's editor shows the controls of the mode it plays in, and the amp only for Classic")
{
    Screen screen;
    auto& editor = *screen.editor;
    CHECK (editor.getModeControls ((int) Sampler::Mode::classic).isVisible());
    CHECK_FALSE (editor.getModeControls ((int) Sampler::Mode::slice).isVisible());

    screen.song.setParameter (screen.sampler, "mode", (float) Sampler::Mode::slice);
    screen.settle();
    CHECK (editor.getModeControls ((int) Sampler::Mode::slice).isVisible());
    CHECK_FALSE (editor.getModeControls ((int) Sampler::Mode::classic).isVisible());
    CHECK (editor.findControl ("root")->getAlpha() < 1.0f); // each slice plays at its own pitch
}

TEST_CASE ("In Slice mode, a click under the sampler's drawing adds a slice, and a double-click on one takes it away")
{
    Screen screen;
    auto& editor = *screen.editor;
    screen.song.setParameter (screen.sampler, "mode", (float) Sampler::Mode::slice);
    screen.song.setParameter (screen.sampler, "sliceBy", (float) Sampler::SliceBy::manual);
    screen.settle();
    REQUIRE (editor.getSliceXs().empty());

    auto& view = editor.getSampleView();
    phazite::test::Mouse mouse (view);
    const auto under = juce::Point<int> (view.getWidth() / 2, view.getHeight() - 5);
    mouse.click (under);
    screen.settle();
    REQUIRE (editor.getSliceXs().size() == 1);
    CHECK (screen.describeUndo() == juce::String ("Undo Add Slice"));
    CHECK (phazite::getSlicePositions (screen.getSong(), screen.sampler).size() == 1);

    const auto x = juce::roundToInt (editor.getSliceXs()[0]);
    mouse.doubleClick ({ x, view.getHeight() / 3 });
    screen.settle();
    CHECK (editor.getSliceXs().empty());
    CHECK (screen.describeUndo() == juce::String ("Undo Remove Slice"));
}

TEST_CASE ("The sampler's editor fits the default dock: the sound alone on its first page, as wide as the dock")
{
    Screen screen;
    auto& editor = *screen.editor;
    constexpr int dockWidth = 552, dockHeight = 268; // beside the Devices view's column
    REQUIRE (editor.getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    editor.setSize (dockWidth, dockHeight);
    CHECK (editor.getLayout().usesPages());
    CHECK (editor.getSampleView().isShowing());
    CHECK (editor.getSampleView().getWidth() > 400);
    phazite::test::checkChildrenApart (editor);

    editor.getLayout().showPage (1);
    CHECK_FALSE (editor.getSampleView().isShowing());
    CHECK (editor.findControl ("transpose")->isShowing());
}
