// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation in the window (roadmap step 2.3), driven with the mouse as the user would: the ring around a modulated knob
// and dragging it, a modulator dropped on a knob, and a modulator's editor with a step sequencer's steps. The track's
// modulators show in the Devices view (DevicesViewTests.cpp).
#include "UiTestHelpers.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/devices/ModulatorEditor.h"

namespace
{
using phazite::ModulatorType;
using phazite::ParameterRef;

// An open project with two tracks, A (with a compressor) and B.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        setEdit (context, edit.get());

        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String ("phaziteCompressor"))
                phazite::addEffect (context, phazite::getTrackID (*a), each);

        edit->getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    ParameterRef threshold() const { return { phazite::getEffects (context, phazite::getTrackID (*a))[0].id, "threshold" }; }
    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
};

// A knob attached to a parameter, in a window.
struct KnobScreen
{
    explicit KnobScreen (Song& s, const ParameterRef& parameter)
        : song (s)
    {
        attachment.setParameter (phazite::getParameter (song.context, parameter));
        attachment.update();
    }

    Song& song;
    phazite::ui::Knob knob { "Threshold", 0.0 };
    phazite::ui::ParameterAttachment attachment { knob };
    phazite::test::TestWindow window { knob, 64, 64 };
};

// A drop of a modulator, as JUCE delivers one.
juce::DragAndDropTarget::SourceDetails dragOf (phazite::ModulatorID modulator, juce::Component& source)
{
    return { phazite::ui::makeModulatorDrag (modulator), &source, {} };
}
} // namespace

TEST_CASE ("A modulated knob shows a ring, which a drag on it changes as one undo step")
{
    Song song;
    KnobScreen screen (song, song.threshold());
    CHECK_FALSE (screen.knob.getModulation().has_value());

    auto handle = phazite::getParameter (song.context, song.threshold());
    handle.setValue (-30.0f); // the middle
    const auto lfo = phazite::modulateWithNew (handle, ModulatorType::lfo, 0.25f);
    REQUIRE (lfo.isValid());
    screen.attachment.update();
    REQUIRE (screen.knob.getModulation().has_value());
    CHECK (screen.knob.getModulation()->low == doctest::Approx (0.25f));
    CHECK (screen.knob.getModulation()->high == doctest::Approx (0.75f));

    // Up from the ring's top edge: more modulation.
    phazite::test::Mouse mouse (screen.knob);
    const juce::Point<int> ring (32, 2);
    mouse.press (ring);
    mouse.dragTo (ring - juce::Point<int> (0, 20));
    mouse.dragTo (ring - juce::Point<int> (0, 40));
    mouse.release();
    mouse.pause();

    const auto modulations = phazite::getModulations (handle);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].amount == doctest::Approx (0.45f).epsilon (0.02));
    CHECK (song.describeUndo() == juce::String ("Undo Change Modulation"));
    CHECK (handle.getValue() == doctest::Approx (-30.0f).epsilon (0.5)); // the knob's own value stays

    // In the middle, a drag still turns the knob.
    mouse.press ({ 32, 32 });
    mouse.dragTo ({ 32, 12 });
    mouse.release();
    CHECK (handle.getBaseValue() > -30.0f);
}

TEST_CASE ("A modulator dropped on a knob of its track modulates it; one of another track's isn't taken")
{
    Song song;
    KnobScreen screen (song, song.threshold());
    const auto lfo = phazite::addModulator (song.context, phazite::getTrackID (*song.a), ModulatorType::lfo);
    const auto elsewhere = phazite::addModulator (song.context, phazite::getTrackID (*song.b), ModulatorType::random);
    juce::Component source;

    CHECK_FALSE (screen.knob.isInterestedInDragSource (dragOf (elsewhere, source)));
    REQUIRE (screen.knob.isInterestedInDragSource (dragOf (lfo, source)));
    screen.knob.itemDropped (dragOf (lfo, source));

    const auto modulations = phazite::getModulations (song.context, song.threshold());
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].modulator == lfo);
    CHECK (modulations[0].amount == doctest::Approx (phazite::defaultModulationAmount));
    CHECK (song.describeUndo() == juce::String ("Undo Modulate Threshold"));

    // Dropped again, it keeps its amount.
    phazite::modulate (song.context, lfo, song.threshold(), 0.6f);
    screen.knob.itemDropped (dragOf (lfo, source));
    CHECK (phazite::getModulations (song.context, song.threshold())[0].amount == doctest::Approx (0.6f));
}

TEST_CASE ("A modulator's editor has a knob for each parameter, and a step sequencer's steps are drawn with the mouse")
{
    Song song;
    const auto lfo = phazite::addModulator (song.context, phazite::getTrackID (*song.a), ModulatorType::lfo);
    phazite::ui::ModulatorEditor lfoEditor { song.context, lfo };
    REQUIRE (lfoEditor.findKnob ("rate") != nullptr);
    REQUIRE (lfoEditor.findKnob ("wave") != nullptr);
    CHECK (lfoEditor.findKnob ("rate")->getTitle() == juce::String ("Rate"));
    CHECK (phazite::test::findComponentWithTitle (lfoEditor, "Output") != nullptr);

    const auto steps = phazite::addModulator (song.context, phazite::getTrackID (*song.a), ModulatorType::steps);
    phazite::ui::ModulatorEditor editor { song.context, steps };
    phazite::test::TestWindow window { editor, editor.getWidth(), editor.getHeight() };
    auto* view = phazite::test::findComponentWithTitle (editor, "Steps");
    REQUIRE (view != nullptr);
    const auto before = phazite::getSteps (song.context, steps);
    REQUIRE (before.size() >= 4);

    // A drag along the top: the steps it passes go all the way up, as one undo step.
    const auto stepWidth = view->getWidth() / (int) before.size();
    phazite::test::Mouse mouse (*view);
    mouse.press ({ stepWidth / 2, 2 });
    mouse.dragTo ({ stepWidth + stepWidth / 2, 2 });
    mouse.dragTo ({ 2 * stepWidth + stepWidth / 2, 2 });
    mouse.release();

    const auto after = phazite::getSteps (song.context, steps);
    CHECK (after[0] == doctest::Approx (1.0f).epsilon (0.1));
    CHECK (after[1] == doctest::Approx (1.0f).epsilon (0.1));
    CHECK (after[2] == doctest::Approx (1.0f).epsilon (0.1));
    CHECK (after[3] == doctest::Approx (before[3]));
    CHECK (song.describeUndo() == juce::String ("Undo Change Steps"));

    phazite::undo (*song.edit);
    CHECK (phazite::getSteps (song.context, steps)[1] == doctest::Approx (before[1]));
}
