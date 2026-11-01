// SPDX-License-Identifier: AGPL-3.0-or-later
// The Devices view (src/ui/mixer/DevicesView.h; roadmap step 2.5), driven as the user would: the selected track's
// devices in a row, each shown whole (an editor for Phazite's own, a container's macros and devices, a plug-in's key
// knobs and a button for its window), which track it shows as the selection changes or a device's name is clicked in
// the mixer, the master's devices, and the track's modulators with an editor beside them.
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Tracktion.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/EqEditor.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/devices/SynthEditor.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/DevicesView.h"
#include "ui/mixer/MixerView.h"
#include "ui/mixer/ModulatorSlots.h"
#include "ui/mixer/StripParts.h"

#include <algorithm>

namespace
{
using phazite::DeviceID;
using phazite::TrackID;

// The editors of a kind in a panel.
template <typename Editor>
int countEditors (juce::Component& panel)
{
    return phazite::test::findAll<Editor> (panel).size();
}

// A drag with the mouse, as the user makes one: pressed, moved in steps, and let go.
void drag (phazite::test::Mouse& mouse, juce::Point<int> from, juce::Point<int> to)
{
    mouse.press (from);

    for (int step = 1; step <= 4; ++step)
        mouse.dragTo (from + (to - from) * step / 4);

    mouse.release();
}

// The knobs that show in a component, by their titles.
juce::StringArray showingKnobs (juce::Component& component)
{
    juce::StringArray titles;

    for (auto* knob : phazite::test::findAll<phazite::ui::Knob> (component))
        if (knob->isShowing())
            titles.add (knob->getTitle());

    return titles;
}

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
        phazite::ensureTracks (*edit, 2);
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
        phazite::getSongUndoManager (*edit).clearUndoHistory();
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
    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::DevicesView view { commands, context };
    phazite::test::TestWindow window { view, 1600, 380 };
};
} // namespace

TEST_CASE ("The Effects page shows the selected track's effects in a row, as cards folded but one")
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

    // Folded at first, narrow; the EQ's chevron opens it, its whole editor with its graph shrunk to fit, and opening the
    // compressor folds it again. The plug-in has a button that opens its own window.
    CHECK (countEditors<phazite::ui::EqEditor> (*eq) == 0);
    CHECK (eq->getWidth() < 130);
    auto* open = phazite::test::findComponentWithTitle (*eq, "Open");
    REQUIRE (open != nullptr);
    phazite::test::Mouse openMouse (*open);
    openMouse.click (open->getLocalBounds().getCentre());
    CHECK (view.isOpen (effects[0]));
    CHECK (countEditors<phazite::ui::EqEditor> (*eq) == 1);
    CHECK (eq->getWidth() > 300);
    view.setOpen (effects[1], true);
    CHECK_FALSE (view.isOpen (effects[0]));
    CHECK (countEditors<phazite::ui::EqEditor> (*eq) == 0);
    CHECK (countEditors<phazite::ui::CompressorEditor> (*compressor) == 1);
    CHECK (phazite::test::findComponentWithTitle (*eq, "Open its window") == nullptr);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open its window") != nullptr);

    // Phazite's devices can show in a window of their own too, when the user wants (app/PluginWindow.h).
    CHECK (phazite::test::findComponentWithTitle (*eq, "Open in a window") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open in a window") == nullptr);
    const auto slots = phazite::getEffects (screen.context, screen.track (0));
    CHECK (slots[0].popsOut);
    CHECK_FALSE (slots[0].windowOpen);
    CHECK_FALSE (slots[2].popsOut);
    CHECK (showingKnobs (*plugin) == juce::StringArray { "Mix" });
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

    // Showing the plug-in opens its card, to its key knobs (the rest are in its window), and folds the one that was open.
    view.showDevice (effects[2]);
    CHECK (view.isOpen (effects[2]));
    CHECK_FALSE (view.isOpen (effects[1]));
    CHECK (showingKnobs (*plugin).size() >= 3);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open its window") != nullptr);
    view.setOpen (effects[1], true);

    // Each page keeps its own cards while the other shows: the same ones come back.
    using Page = phazite::ui::DevicesView::Page;
    view.showPage (Page::instrument);
    view.showPage (Page::effects);
    CHECK (view.findPanel (effects[0]) == eq);

    // An effect just added opens, the others folding, even while the Instrument page shows.
    view.showPage (Page::instrument);
    phazite::addEffect (screen.context, screen.track (0), screen.find ("phaziteDelay"));
    phazite::test::dispatchPendingMessages();
    view.showPage (Page::effects);
    const auto delay = phazite::getEffects (screen.context, screen.track (0)).back().id;
    CHECK (view.isOpen (delay));
    CHECK_FALSE (view.isOpen (effects[1]));
}

TEST_CASE ("A folded card shows only what every effect has, one under another: its switch, name, sidechain and mix")
{
    // The owner (2026-10-30): folded effects showed knobs and pictures squeezed under a header whose parts crowded its top.
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    view.setOpen (effects[1], false); // the compressor, added last, opened

    for (const auto& effect : effects)
    {
        auto* card = view.findPanel (effect);
        REQUIRE (card != nullptr);
        CAPTURE (card->getTitle());
        REQUIRE_FALSE (view.isOpen (effect));

        // No editor, picture or key knob: its mix alone, named.
        CHECK (countEditors<phazite::ui::EqEditor> (*card) == 0);
        CHECK (countEditors<phazite::ui::CompressorEditor> (*card) == 0);
        CHECK (showingKnobs (*card) == juce::StringArray { "Mix" });
        CHECK (phazite::test::findComponentWithTitle (*card, "Compressor history") == nullptr);

        // One under another, in a narrow card: the switch over the name, over the mix.
        auto* power = phazite::test::findComponentWithTitle (*card, "On");
        auto* name = findNameButton (*card, card->getTitle());
        auto* mix = phazite::test::findComponentWithTitle (*card, "Mix");
        REQUIRE (power != nullptr);
        REQUIRE (name != nullptr);
        REQUIRE (mix != nullptr);
        CHECK (card->getWidth() < 130);
        CHECK (power->getBottom() <= name->getY());
        CHECK (name->getBottom() <= mix->getY());
        phazite::test::checkChildrenApart (*card);
    }

    // The compressor's sidechain, between its name and its mix.
    auto* compressor = view.findPanel (effects[1]);
    auto* sidechain = phazite::test::findComponentWithTitle (*compressor, "Sidechain");
    REQUIRE (sidechain != nullptr);
    CHECK (sidechain->getY() >= findNameButton (*compressor, "Compressor")->getBottom());
    CHECK (sidechain->getBottom() <= phazite::test::findComponentWithTitle (*compressor, "Mix")->getY());

    // The chevron opens it, with all its controls, its header's parts along its top again.
    auto* open = phazite::test::findComponentWithTitle (*compressor, "Open");
    REQUIRE (open != nullptr);
    phazite::test::Mouse mouse (*open);
    mouse.click (open->getLocalBounds().getCentre());
    CHECK (view.isOpen (effects[1]));
    CHECK (countEditors<phazite::ui::CompressorEditor> (*compressor) == 1);
    CHECK (phazite::test::findComponentWithTitle (*compressor, "Mix")->getY() < 30);
}

TEST_CASE ("A modulator dragged by its grip lights every knob it can modulate as the drag starts, until it is let go")
{
    Screen screen;
    auto& view = screen.view;
    auto* group = phazite::test::findComponentWithTitle (view, "Modulators");
    REQUIRE (group != nullptr);
    const auto lfo = phazite::addModulator (screen.context, screen.track (0), phazite::ModulatorType::lfo);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "LFO 1") != nullptr; }));

    // Its grip carries a chip with its name, right of and below the pointer.
    auto* grip = dynamic_cast<phazite::ui::strip::ModulatorGrip*> (phazite::test::findComponentWithTitle (*group, "Drag to modulate"));
    REQUIRE (grip != nullptr);
    CHECK (grip->chipText == juce::String ("LFO 1"));
    const auto chip = grip->makeDragImage();
    CHECK (chip.getScaledBounds().getWidth() > 2.0 * phazite::ui::ModulatorDragTint::chipOffsetX);
    CHECK (chip.getScaledBounds().getHeight() > phazite::ui::ModulatorDragTint::chipHeight);

    // A knob of the other track's reverb, in a window of its own, as a device's window would show it.
    phazite::ui::Knob otherKnob { "Mix", 1.0 };
    phazite::ui::ParameterAttachment otherAttachment { otherKnob };
    otherAttachment.setParameter (phazite::getParameter (screen.context, { screen.effects (1)[0], "mix" }));
    phazite::test::TestWindow otherWindow { otherKnob, 64, 64 };

    const auto countLit = [&view]
    {
        const auto knobs = phazite::test::findAll<phazite::ui::Knob> (view);
        return std::count_if (knobs.begin(), knobs.end(), [] (const phazite::ui::Knob* knob) { return knob->isModulatorTarget(); });
    };

    // As the drag starts, as JUCE tells the container the grip is in: every knob of the track's devices that it can modulate
    // lights, before the pointer reaches one; the other track's doesn't.
    auto* slots = grip->findParentComponentOfClass<phazite::ui::ModulatorSlots>();
    REQUIRE (slots != nullptr);
    const juce::DragAndDropTarget::SourceDetails dragged { phazite::ui::makeModulatorDrag (lfo), grip, {} };
    slots->dragOperationStarted (dragged);
    CHECK (countLit() > 2);
    CHECK_FALSE (otherKnob.isModulatorTarget());

    // Let go: none.
    slots->dragOperationEnded (dragged);
    CHECK (countLit() == 0);

    // The other track's modulator lights the other knob, in its window, and none here.
    const auto otherLfo = phazite::addModulator (screen.context, screen.track (1), phazite::ModulatorType::lfo);
    phazite::ui::Knob::showModulatorTargets (otherLfo);
    CHECK (otherKnob.isModulatorTarget());
    CHECK (countLit() == 0);
    phazite::ui::Knob::showModulatorTargets ({});
    CHECK_FALSE (otherKnob.isModulatorTarget());
}

TEST_CASE ("An effect dragged by its name moves among its neighbours, in the Devices view and in the mixer")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0); // the EQ, the compressor, the plug-in
    REQUIRE (effects.size() == 3);
    view.setOpen (effects[1], false);

    // The EQ's name, dragged past the plug-in's card: it goes last, as one undo step.
    auto* eq = view.findPanel (effects[0]);
    auto* plugin = view.findPanel (effects[2]);
    REQUIRE (eq != nullptr);
    REQUIRE (plugin != nullptr);
    auto* eqName = findNameButton (*eq, "EQ");
    REQUIRE (eqName != nullptr);
    phazite::test::Mouse mouse (*eqName);
    drag (mouse, eqName->getLocalBounds().getCentre(), eqName->getLocalPoint (plugin, juce::Point<int> (plugin->getWidth() - 4, 60)));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return screen.effects (0) == std::vector { effects[1], effects[2], effects[0] }; }));
    CHECK (phazite::describeUndo (screen.edit.get()) == juce::String ("Undo Move Device"));
    CHECK (screen.context.getSongSelection().getDevices().empty()); // a drag, not a click: nothing selected

    // A drop where it was moves nothing.
    eq = view.findPanel (effects[0]);
    REQUIRE (eq != nullptr);
    eqName = findNameButton (*eq, "EQ");
    REQUIRE (eqName != nullptr);
    phazite::test::Mouse again (*eqName);
    drag (again, eqName->getLocalBounds().getCentre(), eqName->getLocalBounds().getCentre().translated (8, 0));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.effects (0) == std::vector { effects[1], effects[2], effects[0] });

    // In the mixer, up and down a strip's effects: the EQ back to the top.
    phazite::ui::MixerView mixer { screen.commands, screen.context };
    phazite::test::TestWindow mixerWindow { mixer, 600, 700 };
    phazite::test::dispatchPendingMessages();
    auto* strip = phazite::test::findAll<phazite::ui::ChannelStrip> (mixer)[0];
    auto* slots = phazite::test::findComponentWithTitle (*strip, "Effects");
    REQUIRE (slots != nullptr);
    auto* eqSlot = findNameButton (*slots, "EQ");
    auto* compressorSlot = findNameButton (*slots, "Compressor");
    REQUIRE (eqSlot != nullptr);
    REQUIRE (compressorSlot != nullptr);
    phazite::test::Mouse slotMouse (*eqSlot);
    drag (slotMouse, eqSlot->getLocalBounds().getCentre(), eqSlot->getLocalPoint (compressorSlot, juce::Point<int> (20, 2)));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return screen.effects (0) == effects; }));
}

TEST_CASE ("While devices' windows show, one of Phazite's own folds with its button lit, and a plug-in's card stays as it was")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    REQUIRE (view.findPanel (effects[2]) != nullptr);
    const auto pluginWidth = view.findPanel (effects[2])->getWidth();
    view.setOpen (effects[0], true);
    REQUIRE (countEditors<phazite::ui::EqEditor> (*view.findPanel (effects[0])) == 1);

    // Windows as Tracktion holds them, showing (the app's would be real ones on the screen).
    std::vector<te::Plugin::Ptr> shown;

    for (const auto id : { effects[0], effects[2] })
    {
        auto device = te::findPluginForID (*screen.edit, phazite::toTracktion (id));
        REQUIRE (device != nullptr);
        device->windowState->pluginWindow = std::make_unique<juce::Component>();
        device->windowState->pluginWindow->setVisible (true);
        shown.push_back (device);
    }

    CHECK (phazite::isDeviceWindowOpen (screen.context, effects[2]));
    CHECK_FALSE (phazite::getEffects (screen.context, screen.track (0))[2].windowOpen); // a plug-in's says nothing of its card

    // The cards made again as the track is selected again.
    screen.context.getSongSelection().selectOnly (screen.track (1));
    phazite::test::dispatchPendingMessages();
    screen.context.getSongSelection().selectOnly (screen.track (0));
    phazite::test::dispatchPendingMessages();

    auto* plugin = view.findPanel (effects[2]);
    auto* eq = view.findPanel (effects[0]);
    REQUIRE (plugin != nullptr);
    REQUIRE (eq != nullptr);
    CHECK (plugin->getWidth() == pluginWidth);
    CHECK (phazite::test::findAll<phazite::ui::EqEditor> (*eq).isEmpty());
    auto* windowButton = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*eq, "Open in a window"));
    REQUIRE (windowButton != nullptr);
    CHECK (windowButton->getToggleState());

    for (auto& device : shown)
        device->windowState->pluginWindow = nullptr;
}

TEST_CASE ("The Instrument page shows the track's instrument alone, as wide as the page, or why it has none")
{
    using Page = phazite::ui::DevicesView::Page;
    Screen screen;
    auto& view = screen.view;
    view.showPage (Page::instrument);
    const auto effects = screen.effects (0);

    // Track A has none yet: a button that adds one, and none of its effects.
    const auto buttons = phazite::test::findAll<juce::TextButton> (view);
    CHECK (std::any_of (buttons.begin(), buttons.end(),
                        [] (juce::TextButton* button) { return button->isShowing() && button->getButtonText() == "+ Instrument"; }));
    CHECK (view.findPanel (effects[0]) == nullptr);

    // With the synth: its editor alone, as wide as the page, and its page as the one it is on.
    phazite::chooseInstrument (screen.context, screen.track (0), screen.find ("phaziteSynth"));
    phazite::test::dispatchPendingMessages();
    const auto instrument = phazite::getInstrument (screen.context, screen.track (0));
    REQUIRE (instrument.has_value());
    auto* panel = view.findPanel (instrument->id);
    REQUIRE (panel != nullptr);
    CHECK (phazite::test::findAll<phazite::ui::SynthEditor> (*panel).size() == 1);
    CHECK (panel->getWidth() > 1000);
    CHECK (view.getPageOf (instrument->id) == Page::instrument);
    CHECK (view.getPageOf (effects[0]) == Page::effects);
    phazite::test::checkChildrenApart (view);

    // No switch of its own, and it stays on: the track's switch mutes it (the owner, 2026-11-01).
    CHECK (phazite::test::findComponentWithTitle (*panel, "On") == nullptr);
    phazite::setDeviceOn (screen.context, instrument->id, false);
    CHECK (phazite::getInstrument (screen.context, screen.track (0))->isOn);

    // The effects are on the other page, without it; shown from elsewhere, a device brings its own page.
    view.showPage (Page::effects);
    CHECK (view.findPanel (instrument->id) == nullptr);
    CHECK (view.findPanel (effects[0]) != nullptr);
    view.showDevice (instrument->id);
    CHECK (view.getPage() == Page::instrument);

    // The master has none, and says why.
    phazite::addEffect (screen.context, {}, screen.find ("phaziteLimiter"));
    view.showDevice (phazite::getEffects (screen.context, {}).back().id);
    view.showPage (Page::instrument);
    const auto labels = phazite::test::findAll<juce::Label> (view);
    CHECK (std::any_of (labels.begin(), labels.end(), [] (juce::Label* label)
                        { return label->isShowing() && label->getText().startsWith ("The master has no instrument"); }));
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

TEST_CASE ("A container shows its macros and its devices in line, as cards; a macro dropped on a device's knob modulates it")
{
    Screen screen;
    const auto effects = screen.effects (0);
    const auto container = phazite::groupIntoContainer (screen.context, { effects[0], effects[1] });
    REQUIRE (container.isValid());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return screen.view.findPanel (container) != nullptr; }));

    // Grouped, it opens: its devices inside it, after its eight macros, as cards.
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

    // A card inside it opening widens the container's card to hold it (the compressor's was open before it was grouped,
    // and stays so: it folds first).
    screen.view.setOpen (effects[1], false);
    const auto foldedWidth = panel->getWidth();
    screen.view.setOpen (effects[0], true);
    eq = screen.view.findPanel (effects[0]);
    REQUIRE (eq != nullptr);
    CHECK (panel->getWidth() > foldedWidth + 100);
    CHECK (panel->getLocalArea (eq, eq->getLocalBounds()).getRight() <= panel->getWidth());
    phazite::test::checkChildrenApart (*panel);

    // The first macro, dropped on the compressor's threshold, in its card opened: it modulates it.
    const auto macro = phazite::getContainerMacros (screen.context, container)[0].id;
    screen.view.setOpen (effects[1], true);
    compressor = screen.view.findPanel (effects[1]);
    REQUIRE (compressor != nullptr);
    auto* threshold = phazite::test::findComponentWithTitle (*compressor, "Threshold");
    REQUIRE (threshold != nullptr);
    auto* knob = dynamic_cast<phazite::ui::Knob*> (threshold);
    REQUIRE (knob != nullptr);
    juce::Component source;
    const juce::DragAndDropTarget::SourceDetails drop { phazite::ui::makeModulatorDrag (macro), &source, {} };
    REQUIRE (knob->isInterestedInDragSource (drop));
    knob->itemDropped (drop);
    CHECK (phazite::getModulations (screen.context, macro).size() == 1);

    // Renamed by typing: the container's name, beside its macros.
    juce::Label* name = nullptr;

    for (auto* label : phazite::test::findAll<juce::Label> (*panel))
        if (label->getText() == "Container" && label->isEditableOnDoubleClick())
            name = label;

    REQUIRE (name != nullptr);
    name->setText ("Glue", juce::sendNotification);
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

    // Removing one takes its slot away, and its editor, open beside them: no slot is left to close it.
    mouse.click (lfo->getLocalBounds().getCentre());
    REQUIRE (phazite::test::findAll<phazite::ui::ModulatorEditor> (view).size() == 1);
    phazite::removeModulator (screen.context, phazite::getModulators (screen.context, screen.track (0))[0].id);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "LFO 1") == nullptr; }));
    CHECK (phazite::test::findAll<phazite::ui::ModulatorEditor> (view).isEmpty());
    phazite::test::checkChildrenApart (view);

    // So does undoing a modulator's adding while its editor is open.
    const auto random = phazite::addModulator (screen.context, screen.track (0), phazite::ModulatorType::random);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "Random 1") != nullptr; }));
    auto* randomName = phazite::test::findComponentWithTitle (*group, "Random 1");
    phazite::test::Mouse randomMouse (*randomName);
    randomMouse.click (randomName->getLocalBounds().getCentre());
    REQUIRE (phazite::test::findAll<phazite::ui::ModulatorEditor> (view).size() == 1);
    CHECK (phazite::test::findAll<phazite::ui::ModulatorEditor> (view).getFirst()->getModulator() == random);
    phazite::undo (*screen.edit);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findAll<phazite::ui::ModulatorEditor> (view).isEmpty(); }));
}

TEST_CASE ("A device with a sidechain shows in its header what it listens to: the one place to choose it")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);

    const auto findSidechain = [&view, &effects] (size_t effect)
    {
        auto* panel = view.findPanel (effects[effect]);
        REQUIRE (panel != nullptr);
        return dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "Sidechain"));
    };

    // The EQ has none; the compressor's listens to the sound passing through it, at first.
    CHECK (findSidechain (0) == nullptr);
    auto* button = findSidechain (1);
    REQUIRE (button != nullptr);
    CHECK (button->getButtonText() == juce::String ("Sidechain"));

    // Listening to the other track: its name, once the view has heard.
    phazite::setSidechainSource (screen.context, effects[1], screen.track (1));
    phazite::test::dispatchPendingMessages();
    button = findSidechain (1);
    REQUIRE (button != nullptr);
    const auto other = phazite::getChannel (screen.context, screen.track (1));
    REQUIRE (other.has_value());
    CHECK (button->getButtonText() == other->name);
    phazite::test::checkChildrenApart (view);
}

TEST_CASE ("A mixer slot of a device that turns the sound down shows how far, along its edge, as the sound plays")
{
    Screen screen;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    phazite::ui::ChannelStrip strip { screen.commands, screen.context };
    phazite::test::TestWindow stripWindow { strip, phazite::ui::ChannelStrip::width, 900 };
    strip.setTrack (screen.track (0));

    // The compressor's slot has one, the EQ's and the plug-in's none.
    const auto meters = phazite::test::findAll<phazite::ui::ValueMeter> (strip);
    const auto isReduction = [] (const phazite::ui::ValueMeter* each) { return each->getTitle() == "Gain reduction"; };
    REQUIRE (std::count_if (meters.begin(), meters.end(), isReduction) == 1);
    auto* meter = *std::find_if (meters.begin(), meters.end(), isReduction);
    auto* compressorName = phazite::test::findComponentWithTitle (strip, "Compressor");
    REQUIRE (compressorName != nullptr);
    CHECK (meter->getParentComponent() == compressorName->getParentComponent());
    CHECK (meter->getRight() == compressorName->getParentComponent()->getWidth());
    phazite::test::checkChildrenApart (*meter->getParentComponent());

    // Nothing at first; then 12 dB over the compressor's threshold, at 4:1, settles 9 dB down, as the audio thread plays it.
    juce::SharedResourcePointer<phazite::ui::DisplayTicker> ticker;
    ticker->tickNow();
    CHECK (meter->getValue() == doctest::Approx (0.0f));

    auto* compressor = phazite::getDeviceHandle (screen.context, effects[1]).getProcessor<phazite::devices::Compressor>();
    REQUIRE (compressor != nullptr);
    compressor->prepare (48000.0, 24000);
    std::vector<float> values;

    for (const auto& parameter : phazite::devices::Compressor::info.parameters)
        values.push_back (parameter.defaultValue);

    juce::AudioBuffer<float> buffer (2, 24000);

    for (int i = 0; i < buffer.getNumSamples(); ++i)
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample (channel, i, (i % 2 == 0 ? 1.0f : -1.0f) * juce::Decibels::decibelsToGain (-6.0f));

    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    phazite::devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = buffer.getNumSamples();
    block.parameters = values;
    compressor->process (block);
    ticker->tickNow();
    CHECK (meter->getValue() == doctest::Approx (9.0f).epsilon (0.05));
}

TEST_CASE ("An open card's Presets button asks for its device's presets; a folded card has none, nor has a container")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);

    std::vector<std::pair<DeviceID, juce::String>> asked;
    view.onShowPresets = [&asked] (DeviceID device, const juce::String& name, juce::Component&) { asked.emplace_back (device, name); };

    // Folded, the EQ shows no Presets button; open, it does, along its header.
    view.setOpen (effects[1], false);
    auto* eq = view.findPanel (effects[0]);
    REQUIRE (eq != nullptr);
    auto* folded = phazite::test::findComponentWithTitle (*eq, "Presets");
    CHECK ((folded == nullptr || ! folded->isShowing()));

    view.setOpen (effects[0], true);
    eq = view.findPanel (effects[0]);
    auto* presets = phazite::test::findComponentWithTitle (*eq, "Presets");
    REQUIRE (presets != nullptr);
    CHECK (presets->isShowing());
    CHECK (presets->getY() < 30);
    phazite::test::Mouse mouse (*presets);
    mouse.click (presets->getLocalBounds().getCentre());
    REQUIRE (asked.size() == 1);
    CHECK (asked[0].first == effects[0]);
    CHECK (asked[0].second == juce::String ("EQ"));
    phazite::test::checkChildrenApart (*eq);

    // A container's presets load as new containers, from the browser: its card has no Presets button.
    const auto container = phazite::groupIntoContainer (screen.context, { effects[1] });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return view.findPanel (container) != nullptr; }));
    view.setOpen (container, true);
    auto* containerCard = view.findPanel (container);
    REQUIRE (containerCard != nullptr);

    for (auto* child : containerCard->getChildren())
        CHECK (child->getTitle() != juce::String ("Presets"));
}
