// SPDX-License-Identifier: AGPL-3.0-or-later
// Device editors made of parts (src/ui/devices/DeviceEditorBase.h, DeviceLayout.h), as the synth's is: a control for each
// parameter, named within its part, and columns that show one part at a time under tabs in a short dock.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/SynthEditor.h"

namespace
{
using phazite::devices::Synth;

// A synth as the song's first track's instrument, which the song's session plays.
phazite::DeviceID addSynth (phazite::test::TestSong& song)
{
    auto& s = *phazite::findSong (song.context);
    const auto id = phazite::setInstrument (s, song.firstTrack, phazite::makeOwnDeviceState (s, Synth::typeID));
    REQUIRE (id.isValid());
    phazite::getSession (song.context)->update();
    return id;
}
} // namespace

TEST_CASE ("The synth's editor has a control for each parameter, in groups, and each control changes its parameter")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = addSynth (song);

    auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (s, id));
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
    CHECK (phazite::getValueSetByHand (s, phazite::ParameterRef { id, "cutoff" }) == doctest::Approx (800.0f));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Cutoff"));

    auto* wave = dynamic_cast<juce::ComboBox*> (panel->findControl ("osc1Wave"));
    REQUIRE (wave != nullptr);
    CHECK (wave->getText() == juce::String ("Saw"));
    wave->setSelectedItemIndex (3, juce::sendNotificationSync); // Square
    CHECK (phazite::getValueSetByHand (s, phazite::ParameterRef { id, "osc1Wave" }) == doctest::Approx (3.0f));

    // Undo shows on the panel.
    phazite::undo (s);
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (wave->getText() == juce::String ("Saw"));
}

TEST_CASE ("Narrower than its parts side by side, as the dock is, the synth's editor shows them in pages, each whole, without tabs")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = addSynth (song);

    auto editor = phazite::ui::createDeviceEditor (phazite::makeDeviceHandle (s, id));
    auto* panel = dynamic_cast<phazite::ui::SynthEditor*> (editor.get());
    REQUIRE (panel != nullptr);
    const auto tall = panel->getHeight(), width = panel->getWidth();

    // As wide at any height: a shorter dock never makes it wider.
    const auto low = panel->getMinimumHeight();
    REQUIRE (low <= tall);
    CHECK (panel->getWidthForHeight (low) == panel->getWidthForHeight (tall));

    // At its own size, as a window first shows it, a part to a column in rows: no pages, no tabs.
    phazite::test::TestWindow window (*panel, width, tall);
    auto& layout = panel->getLayout();
    CHECK_FALSE (layout.usesPages());

    for (int column = 0; column < 6; ++column)
        CHECK_FALSE (layout.usesTabs (column));

    // As wide and as tall as the dock at the default window: in pages, two or three parts to each, every page whole.
    constexpr int dockWidth = 552, dockHeight = 268; // beside the Devices view's column
    REQUIRE (panel->getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    window.setSize (dockWidth, dockHeight);
    REQUIRE (layout.usesPages());
    CHECK (layout.getNumPages() == 3);
    CHECK (layout.getPageName (0) == juce::String::fromUTF8 ("Oscillators \xc2\xb7 Filter"));

    for (int column = 0; column < 6; ++column)
        CHECK_FALSE (layout.usesTabs (column));

    phazite::test::checkChildrenApart (*panel);
    auto* unison = panel->findControl ("unisonVoices");
    REQUIRE (unison != nullptr);
    CHECK_FALSE (unison->isShowing());

    // The last page's tab shows the unison and how the synth plays, inside the editor.
    juce::Button* tab = nullptr;

    for (auto* button : phazite::test::findAll<juce::Button> (*panel))
        if (button->getTitle() == layout.getPageName (2))
            tab = button;

    REQUIRE (tab != nullptr);
    phazite::test::Mouse mouse (*tab);
    mouse.click (tab->getLocalBounds().getCentre());
    CHECK (layout.getShownPage() == 2);
    CHECK (unison->isShowing());
    CHECK (panel->getLocalBounds().contains (panel->getLocalArea (unison, unison->getLocalBounds())));
    phazite::test::checkChildrenApart (*panel);

    // Narrower in a taller dock, its columns wrap into rows, every part showing.
    CHECK (panel->getHeightForWidth (width / 2) > tall);
}

TEST_CASE ("The synth's editor drags its filter's dot for the cutoff and resonance, and shows a note's envelope as it plays")
{
    phazite::test::TestSong song;
    auto& s = *phazite::findSong (song.context);
    const auto id = addSynth (song);
    s.getUndoManager().clearUndoHistory();

    const auto device = phazite::makeDeviceHandle (s, id);
    phazite::ui::SynthEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    const auto valueOf = [&s, id] (const char* parameter)
    { return phazite::getValueSetByHand (s, phazite::ParameterRef { id, parameter }); };

    // The dot dragged to the middle: a lower cutoff, the resonance half way, one undo step.
    auto& filter = editor.getFilter();
    const auto cutoffBefore = valueOf ("cutoff");
    phazite::test::Mouse mouse (filter);
    mouse.press (filter.getDotPosition().roundToInt());
    mouse.dragTo (filter.getLocalBounds().getCentre());
    mouse.release();
    CHECK (valueOf ("cutoff") < cutoffBefore);
    CHECK (valueOf ("resonance") == doctest::Approx (0.5f).epsilon (0.1));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Change Cutoff"));

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

    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
    CHECK (editor.getAmpEnvelope().getShownLevel() > 0.1f);
    CHECK (editor.getOscillators().isShowing());
}
