// SPDX-License-Identifier: AGPL-3.0-or-later
// The Devices view (src/ui/mixer/DevicesView.h; roadmap step 2.5), driven as the user would: the selected track's devices
// stacked in its column, the one clicked shown whole beside it (an editor for Phazite's own, a container's macros beside
// its device's, a plug-in's key knobs), which track it shows as the selection changes or a device's name is clicked in the
// mixer, the master's devices, and the track's modulators as chips whose editors pop out; and every built-in device whole
// in the dock at the default window size, without scrolling.
#include "AppScreen.h"
#include "EnginePlayback.h"
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/components/PowerSwitch.h"
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
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <set>

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

// A project context's devices' windows as the app shows them, while this lives, without any on the screen: which ones show.
struct ShownWindows final : phazite::DeviceWindowHost
{
    explicit ShownWindows (phazite::ProjectContext& c)
        : context (c)
    {
        phazite::setDeviceWindows (context, this);
    }

    ~ShownWindows() override { phazite::setDeviceWindows (context, nullptr); }

    void show (DeviceID id) override { open.insert (id); }
    bool isOpen (DeviceID id) const override { return open.contains (id); }
    void close (DeviceID id) override { open.erase (id); }

    phazite::ProjectContext& context;
    std::set<DeviceID> open;
};

// An open project with two tracks: A, with an EQ, a compressor and a plug-in, and B, with a reverb. "device.show" does
// what the app's does: it shows the selected device in the view.
struct Screen
{
    Screen()
    {
        phazite::ensureTracks (*song, 2);
        setSong (context, song.get());

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
        song->getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
    }

    ~Screen() { setSong (context, nullptr); }

    TrackID track (int index) const { return phazite::getTrackIDs (*song)[(size_t) index]; }

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

    phazite::test::TestPlugin testPlugin; // before the song that holds one, and gone after it
    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::DevicesView view { commands, context };
    phazite::test::TestWindow window { view, 1600, 380 };
};
} // namespace

TEST_CASE ("The Effects tab stacks the selected track's effects in the column, as FL Studio's mixer slots; the one clicked shows beside it")
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

    // One under another as the sound goes, in the narrow column at the left; the rest of the view for the one that shows.
    CHECK (eq->getScreenY() < compressor->getScreenY());
    CHECK (compressor->getScreenY() < plugin->getScreenY());
    CHECK (eq->getScreenX() == plugin->getScreenX());
    CHECK (eq->getWidth() < 170);

    // The compressor, added last but the plug-in, shows beside the column, as wide as the rest of the view; a click on the
    // EQ's name shows the EQ in its place, and selects it.
    REQUIRE (view.isOpen (effects[1]));
    auto* body = view.getShownBody();
    REQUIRE (dynamic_cast<phazite::ui::CompressorEditor*> (body) != nullptr);
    CHECK (body->getScreenX() > eq->getScreenX() + eq->getWidth());
    CHECK (body->getWidth() > 1000);
    auto* eqName = findNameButton (*eq, "EQ");
    REQUIRE (eqName != nullptr);
    phazite::test::Mouse eqMouse (*eqName);
    eqMouse.click (eqName->getLocalBounds().getCentre());
    CHECK (view.isOpen (effects[0]));
    CHECK_FALSE (view.isOpen (effects[1]));
    CHECK (dynamic_cast<phazite::ui::EqEditor*> (view.getShownBody()) != nullptr);
    CHECK (screen.context.getSongSelection().getDevices() == std::vector { effects[0] });

    // Ctrl adds another to the selection, and shows nothing else.
    auto* compressorName = findNameButton (*compressor, "Compressor");
    REQUIRE (compressorName != nullptr);
    phazite::test::Mouse compressorMouse (*compressorName);
    compressorMouse.click (compressorName->getLocalBounds().getCentre(), juce::ModifierKeys::commandModifier);
    CHECK (screen.context.getSongSelection().getDevices() == std::vector { effects[0], effects[1] });
    CHECK (view.isOpen (effects[0]));

    // The one that shows has its window's button under its line: Phazite's own opens in a window, a plug-in its own.
    CHECK (phazite::test::findComponentWithTitle (*eq, "Open in a window") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open its window") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*plugin, "Open in a window") == nullptr);
    const auto slots = phazite::getEffects (screen.context, screen.track (0));
    CHECK (slots[0].popsOut);
    CHECK_FALSE (slots[0].windowOpen);
    CHECK_FALSE (slots[2].popsOut);
    CHECK (phazite::test::findComponentWithTitle (view, "Choose instrument") == nullptr); // no notes to play

    // Turning one off by its green switch, as a track's mute switch looks.
    auto* power = phazite::test::findComponentWithTitle (*compressor, "On");
    REQUIRE (power != nullptr);
    CHECK (dynamic_cast<phazite::ui::PowerSwitch*> (power) != nullptr);
    phazite::test::Mouse powerMouse (*power);
    powerMouse.click (power->getLocalBounds().getCentre());
    CHECK_FALSE (phazite::getEffects (screen.context, screen.track (0))[1].isOn);
    CHECK (phazite::describeUndo (screen.song.get()).isNotEmpty());
    phazite::test::checkChildrenApart (*compressor);

    // Showing the plug-in shows its key knobs beside the column (the rest are in its window).
    view.showDevice (effects[2]);
    CHECK (view.isOpen (effects[2]));
    REQUIRE (view.getShownBody() != nullptr);
    CHECK_FALSE (showingKnobs (*view.getShownBody()).isEmpty()); // its mix is in its slot

    // Each page keeps its own while the other shows: the same slots come back, the same one showing.
    using Page = phazite::ui::DevicesView::Page;
    view.showPage (Page::instrument);
    view.showPage (Page::effects);
    CHECK (view.findPanel (effects[0]) == eq);
    CHECK (view.isOpen (effects[2]));

    // An effect just added shows, even while the Instrument page shows.
    view.showPage (Page::instrument);
    phazite::addEffect (screen.context, screen.track (0), screen.find ("phaziteDelay"));
    phazite::test::dispatchPendingMessages();
    view.showPage (Page::effects);
    const auto delay = phazite::getEffects (screen.context, screen.track (0)).back().id;
    CHECK (view.isOpen (delay));
    CHECK_FALSE (view.isOpen (effects[2]));
}

TEST_CASE (
    "An effect's slot has what every effect has along its line, its switch, name and mix; the one that shows, its Presets and sidechain under it")
{
    // The owner (2026-11-06): stacked as FL Studio's mixer slots, in place of a row of cards that grew narrower as it grew.
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    REQUIRE (view.isOpen (effects[1])); // the compressor, added last but the plug-in

    for (const auto& effect : effects)
    {
        auto* slot = view.findPanel (effect);
        REQUIRE (slot != nullptr);
        CAPTURE (slot->getTitle());

        // No editor, picture or key knob in the column: its mix alone.
        CHECK (countEditors<phazite::ui::EqEditor> (*slot) == 0);
        CHECK (countEditors<phazite::ui::CompressorEditor> (*slot) == 0);
        CHECK (showingKnobs (*slot) == juce::StringArray { "Mix" });

        // Along one line: the switch, the name, the mix.
        auto* power = phazite::test::findComponentWithTitle (*slot, "On");
        auto* name = findNameButton (*slot, slot->getTitle());
        auto* mix = phazite::test::findComponentWithTitle (*slot, "Mix");
        REQUIRE (power != nullptr);
        REQUIRE (name != nullptr);
        REQUIRE (mix != nullptr);
        CHECK (power->getRight() <= name->getX());
        CHECK (name->getRight() <= mix->getX());
        CHECK (std::abs (power->getBounds().getCentreY() - name->getBounds().getCentreY()) <= 1);
        phazite::test::checkChildrenApart (*slot);

        // Its Presets under its line, while it shows only.
        auto* presets = phazite::test::findComponentWithTitle (*slot, "Presets");
        REQUIRE (presets != nullptr);
        CHECK (presets->isVisible() == view.isOpen (effect));

        if (view.isOpen (effect))
            CHECK (presets->getY() >= name->getBottom());
    }

    // The compressor's sidechain, under its line with its Presets.
    auto* compressor = view.findPanel (effects[1]);
    auto* sidechain = phazite::test::findComponentWithTitle (*compressor, "Sidechain");
    REQUIRE (sidechain != nullptr);
    CHECK (sidechain->isVisible());
    CHECK (sidechain->getY() >= findNameButton (*compressor, "Compressor")->getBottom());
}

TEST_CASE ("A modulator dragged by its grip lights every knob it can modulate as the drag starts, until it is let go")
{
    Screen screen;
    auto& view = screen.view;
    juce::Component* group = phazite::test::findAll<phazite::ui::ModulatorSlots> (view).getFirst(); // under its heading
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

TEST_CASE ("An effect dragged by its name moves among its neighbours, up and down the column and the mixer's strips")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0); // the EQ, the compressor, the plug-in
    REQUIRE (effects.size() == 3);

    // The EQ's name, dragged past the plug-in's slot: it goes last, as one undo step.
    auto* eq = view.findPanel (effects[0]);
    auto* plugin = view.findPanel (effects[2]);
    REQUIRE (eq != nullptr);
    REQUIRE (plugin != nullptr);
    auto* eqName = findNameButton (*eq, "EQ");
    REQUIRE (eqName != nullptr);
    phazite::test::Mouse mouse (*eqName);
    drag (mouse, eqName->getLocalBounds().getCentre(), eqName->getLocalPoint (plugin, juce::Point<int> (40, plugin->getHeight() - 2)));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return screen.effects (0) == std::vector { effects[1], effects[2], effects[0] }; }));
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Move Device"));
    CHECK (screen.context.getSongSelection().getDevices().empty()); // a drag, not a click: nothing selected

    // A drop where it was moves nothing.
    eq = view.findPanel (effects[0]);
    REQUIRE (eq != nullptr);
    eqName = findNameButton (*eq, "EQ");
    REQUIRE (eqName != nullptr);
    phazite::test::Mouse again (*eqName);
    drag (again, eqName->getLocalBounds().getCentre(), eqName->getLocalBounds().getCentre().translated (0, 8));
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

TEST_CASE ("While devices' windows show, one of Phazite's own shows a word beside the column in its place, its button lit")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);
    view.setOpen (effects[0], true);
    REQUIRE (dynamic_cast<phazite::ui::EqEditor*> (view.getShownBody()) != nullptr);

    // The windows of the EQ and the plug-in, shown as the model asks for them (the app's would be real ones on the screen).
    ShownWindows windows { screen.context };
    phazite::showDeviceWindow (screen.context, effects[0]);
    phazite::showDeviceWindow (screen.context, effects[2]);
    REQUIRE (windows.open == std::set { effects[0], effects[2] });

    CHECK (phazite::isDeviceWindowOpen (screen.context, effects[2]));
    CHECK_FALSE (phazite::getEffects (screen.context, screen.track (0))[2].windowOpen); // a plug-in's says nothing of its slot

    // The slots made again as the track is selected again; the EQ still the one that shows.
    screen.context.getSongSelection().selectOnly (screen.track (1));
    phazite::test::dispatchPendingMessages();
    screen.context.getSongSelection().selectOnly (screen.track (0));
    phazite::test::dispatchPendingMessages();

    // One editor at a time: the window's, and a word in its place beside the column.
    auto* eq = view.findPanel (effects[0]);
    REQUIRE (eq != nullptr);
    CHECK (view.isOpen (effects[0]));
    CHECK (phazite::test::findAll<phazite::ui::EqEditor> (view).isEmpty());
    auto* word = dynamic_cast<juce::Label*> (view.getShownBody());
    REQUIRE (word != nullptr);
    CHECK (word->getText().contains ("window of its own"));
    auto* windowButton = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*eq, "Open in a window"));
    REQUIRE (windowButton != nullptr);
    CHECK (windowButton->getToggleState());
}

TEST_CASE ("The Instrument page shows the track's instrument in the column, its editor beside it as wide as the rest, or why it has none")
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

    // With the synth: its name, Presets and window's button in the column (the owner, 2026-11-06: not a header as wide as
    // the dock), its editor alone beside it, as wide as the rest of the view; and its page as the one it is on.
    phazite::chooseInstrument (screen.context, screen.track (0), screen.find ("phaziteSynth"));
    phazite::test::dispatchPendingMessages();
    const auto instrument = phazite::getInstrument (screen.context, screen.track (0));
    REQUIRE (instrument.has_value());
    auto* panel = view.findPanel (instrument->id);
    REQUIRE (panel != nullptr);
    CHECK (panel->getWidth() < 170);
    CHECK (findNameButton (*panel, "Synth") != nullptr);
    auto* presets = phazite::test::findComponentWithTitle (*panel, "Presets");
    REQUIRE (presets != nullptr);
    CHECK (presets->isShowing());
    CHECK (phazite::test::findComponentWithTitle (*panel, "Open in a window") != nullptr);
    CHECK (dynamic_cast<phazite::ui::SynthEditor*> (view.getShownBody()) != nullptr);
    CHECK (view.getShownBody()->getWidth() > 1000);
    CHECK (view.getPageOf (instrument->id) == Page::instrument);
    CHECK (view.getPageOf (effects[0]) == Page::effects);
    phazite::test::checkChildrenApart (*panel);

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
    setSong (screen.context, nullptr);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (view.getChannel().has_value());
    CHECK (view.findPanel (limiter) == nullptr);
    setSong (screen.context, screen.song.get());
}

TEST_CASE ("A container lists its devices under it in the column; one shown shows beside the container's macros, which drop on its knobs")
{
    Screen screen;
    const auto effects = screen.effects (0);
    const auto container = phazite::groupIntoContainer (screen.context, { effects[0], effects[1] });
    REQUIRE (container.isValid());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return screen.view.findPanel (container) != nullptr; }));

    // Its devices under it in its slot, indented, one under another. Grouped, it shows: its eight macros beside the column,
    // with a word on where its devices are.
    auto* panel = screen.view.findPanel (container);
    auto* eq = screen.view.findPanel (effects[0]);
    auto* compressor = screen.view.findPanel (effects[1]);
    REQUIRE (eq != nullptr);
    REQUIRE (compressor != nullptr);
    CHECK (panel->isParentOf (eq));
    CHECK (eq->getScreenY() < compressor->getScreenY());
    CHECK (eq->getScreenX() > panel->getScreenX());
    CHECK (screen.view.isOpen (container));
    auto* frame = screen.view.getShownBody();
    REQUIRE (frame != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*frame, "Macro 1") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*frame, "Macro 8") != nullptr);
    phazite::test::checkChildrenApart (*frame);

    // Its compressor shown: its editor beside the container's macros, the first of which, dropped on its makeup gain,
    // modulates it.
    screen.view.setOpen (effects[1], true);
    CHECK (screen.view.isOpen (effects[1]));
    CHECK (screen.view.isOpen (container));
    auto* editor = dynamic_cast<phazite::ui::CompressorEditor*> (screen.view.getShownBody());
    REQUIRE (editor != nullptr);
    auto* holder = editor->getParentComponent();
    REQUIRE (holder != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*holder, "Macro 1") != nullptr);
    phazite::test::checkChildrenApart (*holder);
    const auto macro = phazite::getContainerMacros (screen.context, container)[0].id;
    auto* knob = dynamic_cast<phazite::ui::Knob*> (phazite::test::findComponentWithTitle (*editor, "Makeup"));
    REQUIRE (knob != nullptr);
    juce::Component source;
    const juce::DragAndDropTarget::SourceDetails drop { phazite::ui::makeModulatorDrag (macro), &source, {} };
    REQUIRE (knob->isInterestedInDragSource (drop));
    knob->itemDropped (drop);
    CHECK (phazite::getModulations (screen.context, macro).size() == 1);

    // Renamed by typing: the container's name, over its macros.
    juce::Label* name = nullptr;

    for (auto* label : phazite::test::findAll<juce::Label> (*holder))
        if (label->getText() == "Container" && label->isEditableOnDoubleClick())
            name = label;

    REQUIRE (name != nullptr);
    name->setText ("Glue", juce::sendNotification);
    CHECK (phazite::describeUndo (screen.song.get()) == juce::String ("Undo Rename Container"));
    CHECK (phazite::getEffects (screen.context, screen.track (0))[0].name == juce::String ("Glue"));

    // Its chevron folds its devices under it, and shows them again.
    panel = screen.view.findPanel (container);
    REQUIRE (panel != nullptr);
    auto* fold = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "Fold its devices"));
    REQUIRE (fold != nullptr);
    phazite::test::Mouse foldMouse (*fold);
    foldMouse.click (fold->getLocalBounds().getCentre());
    CHECK_FALSE (screen.view.findPanel (effects[0])->isShowing());

    // A device added to it shows in it, and shows beside the column.
    phazite::addEffectToContainer (screen.context, container, screen.find ("phaziteReverb"));
    CHECK (phazite::test::dispatchMessagesUntil (
        [&]
        {
            const auto inside = phazite::getContainerDevices (screen.context, container);
            return inside.size() == 3 && screen.view.findPanel (inside.back().id) != nullptr && screen.view.isOpen (inside.back().id);
        }));
}

TEST_CASE ("The Devices view shows its track's modulators as chips, a macro with its knob; a chip pops its editor out over the window")
{
    Screen screen;
    auto& view = screen.view;
    juce::Component* group = phazite::test::findAll<phazite::ui::ModulatorSlots> (view).getFirst(); // under its heading
    REQUIRE (group != nullptr);
    CHECK (group->isShowing());
    auto* slots = dynamic_cast<phazite::ui::ModulatorSlots*> (group);
    REQUIRE (slots != nullptr);

    phazite::addModulator (screen.context, screen.track (0), phazite::ModulatorType::lfo);
    phazite::addModulator (screen.context, screen.track (0), phazite::ModulatorType::macro);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "LFO 1") != nullptr; }));
    CHECK (phazite::test::findComponentWithTitle (*group, "Macro 1") != nullptr);
    CHECK (phazite::test::findAll<phazite::ui::Knob> (*group).size() == 1); // the macro's
    CHECK (group->getWidth() < 170);                                        // a narrow column, the devices beside it
    phazite::test::checkChildrenApart (*group);

    // A click on the LFO's chip pops its editor out over the window, beside the chip, its chip lit; the devices keep their
    // room. A click on the chip again closes it.
    auto* lfo = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*group, "LFO 1"));
    REQUIRE (lfo != nullptr);
    const auto devicesBefore = phazite::test::findComponentWithTitle (view, "Controls")->getBounds();
    phazite::test::Mouse mouse (*lfo);
    mouse.click (lfo->getLocalBounds().getCentre());
    auto* editor = dynamic_cast<phazite::ui::ModulatorEditor*> (slots->getOpenEditor());
    REQUIRE (editor != nullptr);
    CHECK (editor->isShowing());
    CHECK (editor->findParentComponentOfClass<juce::CallOutBox>() != nullptr);
    CHECK (lfo->getToggleState());
    CHECK (phazite::test::findComponentWithTitle (view, "Controls")->getBounds() == devicesBefore);
    juce::Thread::sleep (250); // a call-out takes a click on what opened it only after a moment, as a touch screen still sends one
    mouse.click (lfo->getLocalBounds().getCentre());
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return slots->getOpenEditor() == nullptr; }));
    slots->updateValues(); // the view's timer does it
    CHECK_FALSE (lfo->getToggleState());

    // The LFO's On switch, in its editor, turns it off, to hear the sound without it; its chip goes faint. The macro has none.
    slots->openEditor (phazite::getModulators (screen.context, screen.track (0))[0].id);
    editor = dynamic_cast<phazite::ui::ModulatorEditor*> (slots->getOpenEditor());
    REQUIRE (editor != nullptr);
    auto* on = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*editor, "On"));
    REQUIRE (on != nullptr);
    CHECK (on->getToggleState());
    phazite::test::Mouse onMouse (*on);
    onMouse.click (on->getLocalBounds().getCentre());
    CHECK_FALSE (phazite::isModulatorOn (screen.context, phazite::getModulators (screen.context, screen.track (0))[0].id));
    slots->updateValues();
    CHECK (lfo->getAlpha() < 1.0f);

    // Removing one takes its chip away, and its editor with it.
    phazite::removeModulator (screen.context, phazite::getModulators (screen.context, screen.track (0))[0].id);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "LFO 1") == nullptr; }));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return slots->getOpenEditor() == nullptr; }));
    phazite::test::checkChildrenApart (*group);

    // So does undoing a modulator's adding while its editor shows. A new modulator's editor opens as it is added.
    const auto random = phazite::addModulator (screen.context, screen.track (0), phazite::ModulatorType::random);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (*group, "Random 1") != nullptr; }));
    slots->openEditor (random);
    editor = dynamic_cast<phazite::ui::ModulatorEditor*> (slots->getOpenEditor());
    REQUIRE (editor != nullptr);
    CHECK (editor->getModulator() == random);
    phazite::undo (*screen.song);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return slots->getOpenEditor() == nullptr; }));
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

    // Played by Phazite's engine, which the devices' readings come from.
    phazite::test::EnginePlayback playback;
    playback.play (0.05);
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

TEST_CASE ("The shown device's Presets button asks for its presets; the other slots show none, nor has a container one")
{
    Screen screen;
    auto& view = screen.view;
    const auto effects = screen.effects (0);
    REQUIRE (effects.size() == 3);

    std::vector<std::pair<DeviceID, juce::String>> asked;
    view.onShowPresets = [&asked] (DeviceID device, const juce::String& name, juce::Component&) { asked.emplace_back (device, name); };

    // The compressor shows: the EQ's slot has no Presets button showing; shown, it does, under its line.
    auto* eq = view.findPanel (effects[0]);
    REQUIRE (eq != nullptr);
    auto* hidden = phazite::test::findComponentWithTitle (*eq, "Presets");
    CHECK ((hidden == nullptr || ! hidden->isShowing()));

    view.setOpen (effects[0], true);
    eq = view.findPanel (effects[0]);
    REQUIRE (eq != nullptr);
    auto* presets = phazite::test::findComponentWithTitle (*eq, "Presets");
    REQUIRE (presets != nullptr);
    CHECK (presets->isShowing());
    CHECK (presets->getY() >= findNameButton (*eq, "EQ")->getBottom());
    phazite::test::Mouse mouse (*presets);
    mouse.click (presets->getLocalBounds().getCentre());
    REQUIRE (asked.size() == 1);
    CHECK (asked[0].first == effects[0]);
    CHECK (asked[0].second == juce::String ("EQ"));
    phazite::test::checkChildrenApart (*eq);

    // A container's presets load as new containers, from the browser: its slot has no Presets button.
    const auto container = phazite::groupIntoContainer (screen.context, { effects[1] });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return view.findPanel (container) != nullptr; }));
    view.setOpen (container, true);
    auto* containerSlot = view.findPanel (container);
    REQUIRE (containerSlot != nullptr);

    for (auto* child : containerSlot->getChildren())
        CHECK (child->getTitle() != juce::String ("Presets"));
}

TEST_CASE ("On a machine without a screen, as Linux CI is, a test mouse letting go gives JUCE a display for that moment only")
{
    // The image of a drag let go fades out by a copy of itself, which JUCE's animator scales for the display it is on; with
    // no display (Linux CI's) it read nothing, and every drag in the tests crashed as it was let go (2026-11-01). A display
    // kept for good would make the machine look as if it had a screen (Desktop::isHeadless()) to what asks later, such as
    // code that turns the screen saver off, which without a screen crashes.
    auto& displays = const_cast<juce::Displays&> (juce::Desktop::getInstance().getDisplays());
    displays.displays.clear();

    {
        Screen screen;
        CHECK (juce::Desktop::getInstance().isHeadless());
        const auto effects = screen.effects (0);
        REQUIRE (effects.size() == 3);
        auto* eq = screen.view.findPanel (effects[0]);
        auto* plugin = screen.view.findPanel (effects[2]);
        REQUIRE (eq != nullptr);
        REQUIRE (plugin != nullptr);
        auto* eqName = findNameButton (*eq, "EQ");
        REQUIRE (eqName != nullptr);
        phazite::test::Mouse mouse (*eqName);
        drag (mouse, eqName->getLocalBounds().getCentre(), eqName->getLocalPoint (plugin, juce::Point<int> (40, plugin->getHeight() - 2)));
        CHECK (phazite::test::dispatchMessagesUntil ([&]
                                                     { return screen.effects (0) == std::vector { effects[1], effects[2], effects[0] }; }));
        CHECK (juce::Desktop::getInstance().isHeadless()); // as it was: none since the drag ended
    }

    displays.refresh(); // the machine's own again, if it has any
}

TEST_CASE ("At the default window size, every built-in instrument and effect shows whole in the dock, without scrolling")
{
    // The whole window at its default size, its panels at theirs, the dock under the playlist, in the app's look (its fonts set
    // the buttons' widths); a track with nothing on it.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.registry.invoke ("track.add"));
    auto& context = screen.session.getContext();
    const auto track = phazite::getTrackIDs (*phazite::findSong (context)).back();
    context.getSongSelection().selectOnly (track);
    phazite::test::dispatchPendingMessages();

    const auto findType = [&context] (const char* id)
    {
        for (const auto& type : phazite::getDeviceTypes (context))
            if (type.description.fileOrIdentifier == juce::String (id))
                return type;

        FAIL ("no " << id);
        return phazite::DeviceType {};
    };

    // The device that shows beside the column shows whole: no scroll bar up and down, nothing cut off at its side.
    const auto checkWhole = [&screen] (const char* type)
    {
        CAPTURE (type);
        phazite::test::dispatchPendingMessages();
        auto* view = phazite::test::findAll<phazite::ui::DevicesView> (screen.shell).getFirst();
        REQUIRE (view != nullptr);
        auto shown = 0;

        for (auto* port : phazite::test::findAll<juce::Viewport> (*view))
        {
            if (port->getTitle() != "Controls" || ! port->isShowing() || port->getViewedComponent() == nullptr)
                continue;

            CHECK (port->getViewedComponent()->getHeight() <= port->getHeight());
            CHECK (port->getViewedComponent()->getWidth() <= port->getWidth());
            CHECK_FALSE (port->getVerticalScrollBar().isVisible());
            ++shown;
        }

        CHECK (shown == 1);
    };

    // Each instrument alone on the Instrument tab.
    phazite::chooseInstrument (context, track, findType ("phaziteSynth"));
    REQUIRE (screen.registry.invoke ("view.instrument"));

    for (const auto* type : { "phaziteSynth", "phaziteWavetable", "phaziteDrums" })
    {
        phazite::chooseInstrument (context, track, findType (type));
        checkWhole (type);
    }

    // Each effect alone in the chain, showing beside the column, on the Effects tab.
    REQUIRE (screen.registry.invoke ("view.effects"));

    for (const auto* type :
         { "phaziteEq", "phaziteCompressor", "phaziteLimiter", "phaziteMultiband", "phaziteSaturator", "phaziteFilter", "phaziteDelay",
           "phaziteReverb", "phazitePhaser", "phaziteFlanger", "phaziteStereoExpander", "phaziteVocoder" })
    {
        phazite::addEffect (context, track, findType (type));
        phazite::test::dispatchPendingMessages();
        const auto effects = phazite::getEffects (context, track);
        REQUIRE_FALSE (effects.empty());
        auto* view = phazite::test::findAll<phazite::ui::DevicesView> (screen.shell).getFirst();
        REQUIRE (view != nullptr);
        view->setOpen (effects.back().id, true);
        checkWhole (type);
        phazite::removeDevice (context, effects.back().id);
    }
}
