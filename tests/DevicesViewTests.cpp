// SPDX-License-Identifier: AGPL-3.0-or-later
// The Devices view (src/ui/mixer/DevicesView.h; roadmap step 2.5), driven as the user would: the selected track's
// devices in a row, each shown whole (an editor for Phazite's own, a container's macros and devices, a plug-in's key
// knobs and a button for its window), which track it shows as the selection changes or a device's name is clicked in
// the mixer, the master's devices, and the track's modulators with an editor beside them.
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "model/base/internal/Tracktion.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/devices/CompressorEditor.h"
#include "ui/devices/EqEditor.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/devices/SynthEditor.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/DevicesView.h"

#include <algorithm>

namespace
{
using phazite::DeviceID;
using phazite::TrackID;

// The editors in a panel that show all of a device: a folded card's shows its picture alone.
template <typename Editor>
int countWholeEditors (juce::Component& panel)
{
    const auto editors = phazite::test::findAll<Editor> (panel);
    return (int) std::count_if (editors.begin(), editors.end(), [] (const Editor* editor) { return ! editor->isPictureOnly(); });
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

    // Folded at first to their key knobs, narrow; the EQ's chevron opens it, its whole editor with its graph shrunk to fit,
    // and opening the compressor folds it again. The plug-in has key knobs and a button that opens its own window.
    CHECK (countWholeEditors<phazite::ui::EqEditor> (*eq) == 0);
    CHECK (eq->getWidth() < 250);
    auto* open = phazite::test::findComponentWithTitle (*eq, "Open");
    REQUIRE (open != nullptr);
    phazite::test::Mouse openMouse (*open);
    openMouse.click (open->getLocalBounds().getCentre());
    CHECK (view.isOpen (effects[0]));
    CHECK (countWholeEditors<phazite::ui::EqEditor> (*eq) == 1);
    CHECK (eq->getWidth() > 300);
    view.setOpen (effects[1], true);
    CHECK_FALSE (view.isOpen (effects[0]));
    CHECK (countWholeEditors<phazite::ui::EqEditor> (*eq) == 0);
    CHECK (countWholeEditors<phazite::ui::CompressorEditor> (*compressor) == 1);
    CHECK (phazite::test::findComponentWithTitle (*eq, "Open its window") == nullptr);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open its window") != nullptr);

    // Phazite's devices can show in a window of their own too, when the user wants (app/PluginWindow.h).
    CHECK (phazite::test::findComponentWithTitle (*eq, "Open in a window") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open in a window") == nullptr);
    const auto slots = phazite::getEffects (screen.context, screen.track (0));
    CHECK (slots[0].popsOut);
    CHECK_FALSE (slots[0].windowOpen);
    CHECK_FALSE (slots[2].popsOut);
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

    // Showing the plug-in, whose card doesn't open, leaves the open card open.
    view.showDevice (effects[2]);
    CHECK (view.isOpen (effects[1]));

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

TEST_CASE ("A folded card shows what its device does as it plays, over its key knobs, and a click on that opens the card")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    auto* eq = view.findPanel (effects[0]);
    auto* compressor = view.findPanel (effects[1]);
    auto* plugin = view.findPanel (effects[2]);
    REQUIRE (eq != nullptr);
    REQUIRE (compressor != nullptr);
    REQUIRE (plugin != nullptr);
    view.setOpen (effects[1], false); // the compressor, added last, opened
    REQUIRE_FALSE (view.isOpen (effects[0]));
    REQUIRE_FALSE (view.isOpen (effects[1]));

    // The EQ's curve alone, filling the card above its knobs; the compressor's history, which shows the ducking, without
    // its curve; the plug-in's card, its knobs only.
    const auto eqPictures = phazite::test::findAll<phazite::ui::EqEditor> (*eq);
    REQUIRE (eqPictures.size() == 1);
    CHECK (eqPictures[0]->isPictureOnly());
    CHECK (eqPictures[0]->isShowing());
    CHECK (eqPictures[0]->getHeight() > 120);
    CHECK (eqPictures[0]->getGraph().getBounds() == eqPictures[0]->getLocalBounds());
    auto* history = phazite::test::findComponentWithTitle (*compressor, "Compressor history");
    REQUIRE (history != nullptr);
    CHECK (history->isShowing());
    auto* curve = phazite::test::findComponentWithTitle (*compressor, "Compressor curve");
    CHECK ((curve == nullptr || ! curve->isShowing()));
    CHECK (phazite::test::findAll<phazite::ui::EqEditor> (*plugin).isEmpty());

    const auto showingKnobs = [] (juce::Component& card)
    {
        auto knobs = phazite::test::findAll<phazite::ui::Knob> (card);
        return std::count_if (knobs.begin(), knobs.end(), [] (const phazite::ui::Knob* knob) { return knob->isShowing(); });
    };

    CHECK (showingKnobs (*compressor) >= 3); // its mix in the header, and its two key knobs
    CHECK (showingKnobs (*eq) >= 3);
    phazite::test::checkChildrenApart (*eq);

    // A click on the picture opens the card, to the whole editor.
    phazite::test::Mouse mouse (*eqPictures[0]);
    mouse.click (eqPictures[0]->getLocalBounds().getCentre());
    CHECK (view.isOpen (effects[0]));
    CHECK (countWholeEditors<phazite::ui::EqEditor> (*eq) == 1);
    CHECK (phazite::test::findAll<phazite::ui::EqEditor> (*eq).size() == 1);

    // A short dock leaves the knobs, and no picture squeezed into what is left.
    view.setOpen (effects[0], false);
    screen.window.setSize (1600, 150);
    auto* shortPicture = phazite::test::findAll<phazite::ui::EqEditor> (*view.findPanel (effects[0]))[0];
    CHECK_FALSE (shortPicture->isShowing());
}

TEST_CASE ("While devices' windows show, a plug-in's card keeps its knobs, and one of Phazite's own folds with its button lit")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    REQUIRE (view.findPanel (effects[2]) != nullptr);
    const auto pluginWidth = view.findPanel (effects[2])->getWidth();
    view.setOpen (effects[0], true);
    REQUIRE (countWholeEditors<phazite::ui::EqEditor> (*view.findPanel (effects[0])) == 1);

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
    const auto reductions = std::count_if (meters.begin(), meters.end(),
                                           [] (const phazite::ui::ValueMeter* meter) { return meter->getTitle() == "Gain reduction"; });
    REQUIRE (reductions == 1);
    auto* meter = *std::find_if (meters.begin(), meters.end(),
                                 [] (const phazite::ui::ValueMeter* meter) { return meter->getTitle() == "Gain reduction"; });
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
