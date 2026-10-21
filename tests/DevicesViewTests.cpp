// SPDX-License-Identifier: AGPL-3.0-or-later
// The Devices view (src/ui/mixer/DevicesView.h; roadmap step 2.5), driven as the user would: the selected track's
// devices in a row, each shown whole (an editor for Phazite's own, a container's macros and devices, a plug-in's key
// knobs and a button for its window), which track it shows as the selection changes or a device's name is clicked in
// the mixer, the master's devices, and the track's modulators with an editor beside them.
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/devices/EqEditor.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/DevicesView.h"

#include <algorithm>

namespace
{
using phazite::DeviceID;
using phazite::TrackID;

// A device's name in its panel's header (the panel has the same title, for screen readers).
juce::Component* findNameButton (juce::Component& panel, const juce::String& name)
{
    for (auto* button : phazite::test::findAll<juce::TextButton> (panel))
        if (button->getTitle() == name)
            return button;

    return nullptr;
}

// An open project with two tracks: A, with an EQ, a compressor and a plug-in, and B, with a reverb. "device.show" does
// what the app's does: it shows the selected device in the view.
struct Screen
{
    Screen()
    {
        edit->ensureNumberOfAudioTracks (2);
        setEdit (context, edit.get());

        for (const auto* type : { "phaziteEq", "phaziteCompressor", "PhaziteTestEffect" })
            phazite::addEffect (context, track (0), find (type));

        phazite::addEffect (context, track (1), find ("phaziteReverb"));

        registry.add ({ .id = "device.show",
                        .name = "Show Device",
                        .perform = [this]
                        {
                            if (const auto devices = context.getSongSelection().getDevices(); ! devices.empty())
                                view.showDevice (devices.front());
                        } });

        context.getSongSelection().selectOnly (track (0));
        edit->getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
    }

    ~Screen() { setEdit (context, nullptr); }

    TrackID track (int index) const { return phazite::getTrackID (*te::getAudioTracks (*edit)[index]); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    std::vector<DeviceID> effects (int index) const
    {
        std::vector<DeviceID> ids;

        for (const auto& effect : phazite::getEffects (context, track (index)))
            ids.push_back (effect.id);

        return ids;
    }

    phazite::test::TestPlugin testPlugin; // before the Edit that holds one, and gone after it
    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::DevicesView view { commands, context };
    phazite::test::TestWindow window { view, 1600, 380 };
};
} // namespace

TEST_CASE ("The Devices view shows the selected track's devices in a row, each whole")
{
    Screen screen;
    auto& view = screen.view;
    REQUIRE (view.getChannel() == std::optional (screen.track (0)));

    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    auto* eq = view.findPanel (effects[0]);
    auto* compressor = view.findPanel (effects[1]);
    auto* plugin = view.findPanel (effects[2]);
    REQUIRE (eq != nullptr);
    REQUIRE (compressor != nullptr);
    REQUIRE (plugin != nullptr);

    // Left to right as the sound goes, each as tall as the view.
    CHECK (eq->getScreenX() < compressor->getScreenX());
    CHECK (compressor->getScreenX() < plugin->getScreenX());
    CHECK (eq->getScreenY() == plugin->getScreenY());
    CHECK (eq->getHeight() > 300);

    // Phazite's EQ shows its editor, its graph shrunk to fit; the plug-in has key knobs and a button for its own window,
    // which Phazite's devices don't have.
    CHECK (phazite::test::findAll<phazite::ui::EqEditor> (*eq).size() == 1);
    CHECK (phazite::test::findComponentWithTitle (*eq, "Open its window") == nullptr);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open its window") != nullptr);
    CHECK (phazite::test::findAll<phazite::ui::Knob> (*plugin).size() >= 2);
    CHECK (phazite::test::findComponentWithTitle (view, "Choose instrument") == nullptr); // no notes to play

    // A plain click on a device's name selects it alone; Ctrl adds another.
    auto* eqName = findNameButton (*eq, "EQ");
    auto* compressorName = findNameButton (*compressor, "Compressor");
    REQUIRE (eqName != nullptr);
    REQUIRE (compressorName != nullptr);
    phazite::test::Mouse eqMouse (*eqName), compressorMouse (*compressorName);
    eqMouse.click (eqName->getLocalBounds().getCentre());
    CHECK (screen.context.getSongSelection().getDevices() == std::vector { effects[0] });
    compressorMouse.click (compressorName->getLocalBounds().getCentre(), juce::ModifierKeys::commandModifier);
    CHECK (screen.context.getSongSelection().getDevices() == std::vector { effects[0], effects[1] });

    // Turning one off from its header.
    auto* power = phazite::test::findComponentWithTitle (*compressor, "On");
    REQUIRE (power != nullptr);
    phazite::test::Mouse powerMouse (*power);
    powerMouse.click (power->getLocalBounds().getCentre());
    CHECK_FALSE (phazite::getEffects (screen.context, screen.track (0))[1].isOn);
    CHECK (phazite::describeUndo (screen.edit.get()).isNotEmpty());
    phazite::test::checkChildrenApart (view);
}

TEST_CASE ("The Devices view follows the selection, and a click on a device's name in the mixer shows it there")
{
    Screen screen;
    auto& view = screen.view;

    // Another track selected: its devices.
    screen.context.getSongSelection().selectOnly (screen.track (1));
    phazite::test::dispatchPendingMessages();
    CHECK (view.getChannel() == std::optional (screen.track (1)));
    CHECK (view.findPanel (screen.effects (1)[0]) != nullptr);
    CHECK (view.findPanel (screen.effects (0)[0]) == nullptr);

    // A device's name clicked in a strip of track A: the view shows A's devices, that one selected.
    phazite::ui::ChannelStrip strip { screen.commands, screen.context };
    phazite::test::TestWindow stripWindow { strip, phazite::ui::ChannelStrip::width, 900 };
    strip.setTrack (screen.track (0));
    auto* compressorName = phazite::test::findComponentWithTitle (strip, "Compressor");
    REQUIRE (compressorName != nullptr);
    phazite::test::Mouse mouse (*compressorName);
    mouse.click (compressorName->getLocalBounds().getCentre());
    CHECK (screen.context.getSongSelection().getDevices() == std::vector { screen.effects (0)[1] });
    CHECK (view.getChannel() == std::optional (screen.track (0)));
    CHECK (view.findPanel (screen.effects (0)[1]) != nullptr);

    // The master's devices, when one of them is chosen.
    phazite::addEffect (screen.context, {}, screen.find ("phaziteLimiter"));
    const auto limiter = phazite::getEffects (screen.context, {}).back().id;
    view.showDevice (limiter);
    CHECK (view.getChannel() == std::optional (TrackID()));
    CHECK (view.findPanel (limiter) != nullptr);
    const auto labels = phazite::test::findAll<juce::Label> (view);
    CHECK (std::any_of (labels.begin(), labels.end(), [] (juce::Label* label) { return label->getText() == juce::String ("Master"); }));

    // Without a project, nothing.
    setEdit (screen.context, nullptr);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (view.getChannel().has_value());
    CHECK (view.findPanel (limiter) == nullptr);
    setEdit (screen.context, screen.edit.get());
}

TEST_CASE ("A container shows its macros and its devices in line; a macro dropped on a device's knob modulates it")
{
    Screen screen;
    const auto effects = screen.effects (0);
    const auto container = phazite::groupIntoContainer (screen.context, { effects[0], effects[1] });
    REQUIRE (container.isValid());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return screen.view.findPanel (container) != nullptr; }));

    // Its devices show whole inside it, after its eight macros.
    auto* panel = screen.view.findPanel (container);
    auto* eq = screen.view.findPanel (effects[0]);
    auto* compressor = screen.view.findPanel (effects[1]);
    REQUIRE (eq != nullptr);
    REQUIRE (compressor != nullptr);
    CHECK (panel->isParentOf (eq));
    CHECK (eq->getScreenX() < compressor->getScreenX());
    CHECK (phazite::test::findComponentWithTitle (*panel, "Macro 1") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*panel, "Macro 8") != nullptr);
    phazite::test::checkChildrenApart (*panel);

    // The first macro, dropped on the compressor's threshold: it modulates it.
    const auto macro = phazite::getContainerMacros (screen.context, container)[0].id;
    auto* threshold = phazite::test::findComponentWithTitle (*compressor, "Threshold");
    REQUIRE (threshold != nullptr);
    auto* knob = dynamic_cast<phazite::ui::Knob*> (threshold);
    REQUIRE (knob != nullptr);
    juce::Component source;
    const juce::DragAndDropTarget::SourceDetails drop { phazite::ui::makeModulatorDrag (macro), &source, {} };
    REQUIRE (knob->isInterestedInDragSource (drop));
    knob->itemDropped (drop);
    CHECK (phazite::getModulations (screen.context, macro).size() == 1);

    // Renamed by typing: the container.
    auto* name = phazite::test::findComponentWithTitle (*panel, "Container");
    REQUIRE (name != nullptr);
    auto labels = phazite::test::findAll<juce::Label> (*name);
    REQUIRE_FALSE (labels.isEmpty());
    labels.getFirst()->setText ("Glue", juce::sendNotification);
    CHECK (phazite::describeUndo (screen.edit.get()) == juce::String ("Undo Rename Container"));
    CHECK (phazite::getEffects (screen.context, screen.track (0))[0].name == juce::String ("Glue"));

    // A device added to it shows in it.
    phazite::addEffectToContainer (screen.context, container, screen.find ("phaziteReverb"));
    CHECK (phazite::test::dispatchMessagesUntil (
        [&]
        {
            const auto inside = phazite::getContainerDevices (screen.context, container);
            return inside.size() == 3 && screen.view.findPanel (inside.back().id) != nullptr;
        }));
}

TEST_CASE ("The Devices view shows its track's modulators, a macro with its knob, and a modulator's editor beside them")
{
    Screen screen;
    auto& view = screen.view;
    auto* group = phazite::test::findComponentWithTitle (view, "Modulators");
    REQUIRE (group != nullptr);
    CHECK (group->isShowing());

    phazite::addModulator (screen.context, screen.track (0), phazite::ModulatorType::lfo);
    phazite::addModulator (screen.context, screen.track (0), phazite::ModulatorType::macro);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "LFO 1") != nullptr; }));
    CHECK (phazite::test::findComponentWithTitle (*group, "Macro 1") != nullptr);
    CHECK (phazite::test::findAll<phazite::ui::Knob> (*group).size() == 1); // the macro's

    // A click on the LFO's name opens its editor beside the modulators, and another click closes it.
    auto* lfo = phazite::test::findComponentWithTitle (*group, "LFO 1");
    phazite::test::Mouse mouse (*lfo);
    mouse.click (lfo->getLocalBounds().getCentre());
    auto editors = phazite::test::findAll<phazite::ui::ModulatorEditor> (view);
    REQUIRE (editors.size() == 1);
    CHECK (editors.getFirst()->isShowing());
    CHECK (editors.getFirst()->getScreenX() > group->getScreenX());
    phazite::test::checkChildrenApart (view);
    mouse.click (lfo->getLocalBounds().getCentre());
    CHECK (phazite::test::findAll<phazite::ui::ModulatorEditor> (view).isEmpty());

    // The LFO's On switch turns it off, to hear the sound without it; the macro has none.
    auto* on = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*group, "On"));
    REQUIRE (on != nullptr);
    CHECK (on->getToggleState());
    phazite::test::Mouse onMouse (*on);
    onMouse.click (on->getLocalBounds().getCentre());
    CHECK_FALSE (phazite::isModulatorOn (screen.context, phazite::getModulators (screen.context, screen.track (0))[0].id));
    CHECK_FALSE (on->getToggleState());

    // Removing one takes its slot away.
    phazite::removeModulator (screen.context, phazite::getModulators (screen.context, screen.track (0))[0].id);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "LFO 1") == nullptr; }));
}
