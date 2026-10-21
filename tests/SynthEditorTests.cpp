// SPDX-License-Identifier: AGPL-3.0-or-later
// Device editors made of parts (src/ui/devices/DeviceEditorBase.h, DeviceLayout.h), as the synth's is: a control for each
// parameter, named within its part, and columns that show one part at a time under tabs in a short dock.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/SynthEditor.h"

using phazite::devices::Synth;

TEST_CASE ("The synth's editor has a control for each parameter, in groups, and each control changes its parameter")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
    REQUIRE (phazite::setInstrument (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();

    auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*plugin));
    auto* panel = dynamic_cast<phazite::ui::SynthEditor*> (editor.get());
    REQUIRE (panel != nullptr);
    phazite::test::TestWindow window (*panel, panel->getWidth(), panel->getHeight());
    phazite::test::checkChildrenApart (*panel);

    // Every parameter has its control: a list for a choice, a knob for the rest, titled with its whole name.
    for (const auto& parameter : Synth::info.parameters)
    {
        CAPTURE (juce::String (parameter.id.data(), parameter.id.size()).toStdString());
        auto* control = panel->findControl (parameter.id);
        REQUIRE (control != nullptr);
        CHECK (control->getTitle() == juce::String (parameter.name.data(), parameter.name.size()));
        CHECK ((dynamic_cast<juce::ComboBox*> (control) != nullptr) == (parameter.unit == phazite::devices::Unit::choice));
    }

    // Under "Filter Envelope", the filter's attack is just "Attack".
    juce::StringArray names;

    for (auto* label : phazite::test::findAll<juce::Label> (*panel))
        names.add (label->getText());

    CHECK (phazite::test::findComponentWithTitle (*panel, "Filter Envelope") != nullptr);
    CHECK (names.contains ("Shape")); // the LFO's
    CHECK_FALSE (names.contains ("Filter Attack"));

    // A knob and a list each change their parameter, as an undo step the model names.
    auto* cutoff = dynamic_cast<phazite::ui::Knob*> (panel->findControl ("cutoff"));
    REQUIRE (cutoff != nullptr);
    cutoff->setValue (800.0, juce::sendNotificationSync);
    CHECK (plugin->getAutomatableParameterByID ("cutoff")->getCurrentValue() == doctest::Approx (800.0f));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Cutoff"));

    auto* wave = dynamic_cast<juce::ComboBox*> (panel->findControl ("osc1Wave"));
    REQUIRE (wave != nullptr);
    CHECK (wave->getText() == juce::String ("Saw"));
    wave->setSelectedItemIndex (3, juce::sendNotificationSync); // Square
    CHECK (plugin->getAutomatableParameterByID ("osc1Wave")->getCurrentValue() == doctest::Approx (3.0f));

    // Undo shows on the panel.
    phazite::undo (*edit);
    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (wave->getText() == juce::String ("Saw"));
}

TEST_CASE ("Given less height, as a Devices view gives it, a column's groups show one at a time under tabs, as wide as before")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
    REQUIRE (phazite::setInstrument (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();

    auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (*plugin));
    auto* panel = dynamic_cast<phazite::ui::SynthEditor*> (editor.get());
    REQUIRE (panel != nullptr);
    const auto tall = panel->getHeight(), width = panel->getWidth();

    // As wide at any height: a shorter dock never makes it wider.
    const auto low = panel->getMinimumHeight();
    REQUIRE (low < tall);
    CHECK (panel->getWidthForHeight (tall) == width);
    CHECK (panel->getWidthForHeight (low) == width);

    // At its own height, every group stacked; at the least, the first column's under tabs, the first showing.
    phazite::test::TestWindow window (*panel, width, tall);
    CHECK_FALSE (panel->getLayout().usesTabs (0));
    window.setSize (width, low);
    CHECK (panel->getLayout().usesTabs (0));
    CHECK (panel->getLayout().getShownPart (0) == 0);
    phazite::test::checkChildrenApart (*panel);

    auto* unison = panel->findControl ("unisonVoices");
    REQUIRE (unison != nullptr);
    CHECK_FALSE (unison->isShowing());

    // The Unison tab shows its controls, inside the panel.
    juce::Button* tab = nullptr;

    for (auto* button : phazite::test::findAll<juce::TextButton> (*panel))
        if (button->getTitle() == "Unison")
            tab = button;

    REQUIRE (tab != nullptr);
    phazite::test::Mouse mouse (*tab);
    mouse.click (tab->getLocalBounds().getCentre());
    CHECK (panel->getLayout().getShownPart (0) == 1);
    CHECK (unison->isShowing());
    CHECK (unison->getParentComponent()->getBottom() <= low);
    phazite::test::checkChildrenApart (*panel);

    // Narrower, as the Instrument tab may be, its columns wrap, every group showing.
    CHECK (panel->getHeightForWidth (width / 2) > tall);
}

TEST_CASE ("The synth's editor drags its filter's dot for the cutoff and resonance, and shows a note's envelope as it plays")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    const auto plugin = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
    REQUIRE (phazite::setInstrument (*te::getAudioTracks (*edit)[0], plugin) != nullptr);
    phazite::test::settle();
    edit->getUndoManager().clearUndoHistory();

    const auto device = phazite::makeDeviceHandle (*plugin);
    phazite::ui::SynthEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    const auto valueOf = [&plugin] (const char* id) { return plugin->getAutomatableParameterByID (id)->getCurrentValue(); };

    // The dot dragged to the middle: a lower cutoff, the resonance half way, one undo step.
    auto& filter = editor.getFilter();
    const auto cutoffBefore = valueOf ("cutoff");
    phazite::test::Mouse mouse (filter);
    mouse.press (filter.getDotPosition().roundToInt());
    mouse.dragTo (filter.getLocalBounds().getCentre());
    mouse.release();
    CHECK (valueOf ("cutoff") < cutoffBefore);
    CHECK (valueOf ("resonance") == doctest::Approx (0.5f).epsilon (0.1));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Change Cutoff"));

    // A note held: the amp envelope shows how far it has got, and the oscillators' wave is drawn.
    auto* synth = device.getProcessor<Synth>();
    REQUIRE (synth != nullptr);
    synth->prepare (48000.0, 4800);
    std::vector<float> values;

    for (const auto& parameter : Synth::info.parameters)
        values.push_back (parameter.defaultValue);

    juce::AudioBuffer<float> buffer (2, 4800);
    buffer.clear();
    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    const phazite::devices::NoteEvent note { .type = phazite::devices::NoteEvent::Type::noteOn, .sample = 0, .number = 60, .value = 1.0f };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = buffer.getNumSamples();
    block.parameters = values;
    block.events = { &note, 1 };
    synth->process (block);

    juce::SharedResourcePointer<phazite::ui::DeviceTicker>()->tickNow();
    CHECK (editor.getAmpEnvelope().getShownLevel() > 0.1f);
    CHECK (editor.getOscillators().isShowing());
}
