// SPDX-License-Identifier: AGPL-3.0-or-later
// Mixer depth (roadmap step 2.9; src/model/mixer/MixerModel.h): a send knob for every return, which its first turn makes;
// where a channel's sound goes, the master or a bus; a device's sidechain by its ID, as its menu picks it; each channel's
// CPU load; the routing view's wires (src/ui/mixer/RoutingView.h), dragged and taken away; and a strip's send knobs and
// output button.
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/RoutingView.h"

namespace
{
// An open project with Kick, Bass and Keys, a bus folder Drums around Kick, and two returns, Reverb and Delay.
struct Mix
{
    Mix()
    {
        edit->ensureNumberOfAudioTracks (3);
        const auto tracks = te::getAudioTracks (*edit);
        kick = tracks[0];
        bass = tracks[1];
        keys = tracks[2];
        kick->setName ("Kick");
        bass->setName ("Bass");
        keys->setName ("Keys");

        drums = phazite::groupIntoFolder ({ kick }, nullptr);
        REQUIRE (drums != nullptr);
        drums->setName ("Drums");

        reverb = phazite::addReturnTrack (*edit, "Reverb", nullptr);
        delay = phazite::addReturnTrack (*edit, "Delay", nullptr);
        REQUIRE (reverb != nullptr);
        REQUIRE (delay != nullptr);

        // A compressor on the bass, which can listen to the kick.
        compressor = phazite::insertDevice (*bass, edit->getPluginCache().createNewPlugin (phazite::devices::Compressor::typeID, {}));
        REQUIRE (compressor != nullptr);

        edit->getUndoManager().clearUndoHistory();
        setEdit (context, edit.get());
    }

    ~Mix() { setEdit (context, nullptr); }

    phazite::TrackID id (te::Track* track) const { return phazite::getTrackID (*track); }
    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack *kick = nullptr, *bass = nullptr, *keys = nullptr, *reverb = nullptr, *delay = nullptr;
    te::FolderTrack* drums = nullptr;
    te::Plugin* compressor = nullptr;
};

juce::StringArray namesOf (const std::vector<phazite::OutputChoice>& choices)
{
    juce::StringArray names;

    for (const auto& choice : choices)
        names.add (choice.name + (choice.chosen ? "*" : ""));

    return names;
}
} // namespace

TEST_CASE ("A send knob for every return: no send until its first turn, which makes one as one undo step")
{
    Mix mix;
    const auto targets = phazite::getSendTargets (mix.context, mix.id (mix.keys));
    REQUIRE (targets.size() == 2);
    CHECK (targets[0].name == juce::String ("Reverb"));
    CHECK (targets[1].name == juce::String ("Delay"));
    CHECK_FALSE (targets[0].send.has_value());

    auto level = phazite::getSendLevelTo (mix.context, mix.id (mix.keys), mix.id (mix.reverb));
    CHECK (level.isValid());
    CHECK_FALSE (level.exists());
    CHECK (level.getValue() == doctest::Approx (0.0f));
    CHECK (level.getRange().end == doctest::Approx (1.0f));

    // A press without a drag makes nothing.
    level.beginGesture();
    level.endGesture();
    CHECK (phazite::getSends (*mix.keys).isEmpty());
    CHECK_FALSE (mix.edit->getUndoManager().canUndo());

    // The first turn makes the send, after the fader, at the level turned to: one step.
    level.beginGesture();
    level.setValue (0.3f);
    level.setValue (0.6f);
    level.endGesture();
    REQUIRE (phazite::getSends (*mix.keys).size() == 1);
    CHECK (level.exists());
    CHECK (level.getValue() == doctest::Approx (0.6f));
    CHECK_FALSE (phazite::isPreFader (*phazite::getSends (*mix.keys)[0]));
    CHECK (mix.describeUndo() == juce::String ("Undo Send to Reverb"));

    const auto withSend = phazite::getSendTargets (mix.context, mix.id (mix.keys));
    REQUIRE (withSend[0].send.has_value());
    CHECK (withSend[0].send->returnName == juce::String ("Reverb"));

    // Later turns change its level.
    level.setValue (0.4f);
    CHECK (mix.describeUndo() == juce::String ("Undo Change Send Level"));

    phazite::undo (*mix.edit);
    phazite::undo (*mix.edit);
    CHECK (phazite::getSends (*mix.keys).isEmpty());
    CHECK_FALSE (level.exists());
    CHECK (level.isValid()); // a turn makes it again

    // A value set outside a drag (a key press) makes it too.
    level.setValue (0.5f);
    CHECK (phazite::getSends (*mix.keys).size() == 1);
    CHECK (mix.describeUndo() == juce::String ("Undo Send to Reverb"));

    // A return sends to the others, not to itself, nor to one whose sound comes back to it; folders don't send.
    phazite::getSendLevelTo (mix.context, mix.id (mix.delay), mix.id (mix.reverb)).setValue (0.5f);
    const auto fromReverb = phazite::getSendTargets (mix.context, mix.id (mix.reverb));
    CHECK (fromReverb.empty());
    CHECK_FALSE (phazite::getSendLevelTo (mix.context, mix.id (mix.reverb), mix.id (mix.delay)).isValid());
    CHECK (phazite::getSendTargets (mix.context, mix.id (mix.drums)).empty());
}

TEST_CASE ("Where a channel's sound goes: the master, or a bus, which the track goes into as a folder")
{
    Mix mix;
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.id (mix.bass))) == juce::StringArray { "Master*", "Drums" });
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.id (mix.kick))) == juce::StringArray { "Master", "Drums*" });
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.id (mix.drums))) == juce::StringArray { "Master*" });
    CHECK (phazite::getOutputChoices (mix.context, mix.id (mix.reverb)).empty());
    CHECK_FALSE (phazite::canChooseOutput (mix.context, mix.id (mix.reverb)));
    CHECK (phazite::canChooseOutput (mix.context, mix.id (mix.bass)));

    phazite::setOutput (mix.context, mix.id (mix.bass), mix.id (mix.drums));
    CHECK (mix.bass->getParentFolderTrack() == mix.drums);
    CHECK (mix.describeUndo() == juce::String ("Undo Change Output"));
    CHECK (phazite::getChannel (mix.context, mix.id (mix.bass))->outputBus == juce::String ("Drums"));

    phazite::setOutput (mix.context, mix.id (mix.bass), {});
    CHECK (mix.bass->getParentFolderTrack() == nullptr);
    CHECK (phazite::getChannel (mix.context, mix.id (mix.bass))->outputBus.isEmpty());

    // Choosing what is chosen already, or a plain folder, changes nothing.
    phazite::setOutput (mix.context, mix.id (mix.bass), {});
    CHECK (mix.describeUndo() == juce::String ("Undo Change Output"));
    phazite::undo (*mix.edit);
    phazite::undo (*mix.edit);
    CHECK (mix.bass->getParentFolderTrack() == nullptr);

    phazite::setMixedAsBus (mix.context, mix.id (mix.drums), false);
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.id (mix.bass))) == juce::StringArray { "Master*" });
    phazite::setOutput (mix.context, mix.id (mix.bass), mix.id (mix.drums));
    CHECK (mix.bass->getParentFolderTrack() == nullptr);
}

TEST_CASE ("A device's sidechain by its ID, and the routing's wires: outputs, sends and sidechains")
{
    Mix mix;
    const auto compressor = phazite::getDeviceID (*mix.compressor);
    CHECK (phazite::hasSidechain (mix.context, compressor));
    CHECK_FALSE (phazite::getSidechainSource (mix.context, compressor).isValid());

    const auto choices = phazite::getSidechainChoices (mix.context, compressor);
    CHECK (std::any_of (choices.begin(), choices.end(), [&mix] (const auto& c) { return c.track == mix.id (mix.kick); }));
    CHECK (std::none_of (choices.begin(), choices.end(), [&mix] (const auto& c) { return c.track == mix.id (mix.bass); }));

    phazite::setSidechainSource (mix.context, compressor, mix.id (mix.kick));
    CHECK (phazite::getSidechainSource (mix.context, compressor) == mix.id (mix.kick));
    CHECK (mix.describeUndo() == juce::String ("Undo Sidechain from Kick"));

    phazite::getSendLevelTo (mix.context, mix.id (mix.keys), mix.id (mix.reverb)).setValue (0.5f);
    const auto send = phazite::getSendTargets (mix.context, mix.id (mix.keys))[0].send;
    REQUIRE (send.has_value());

    const auto connections = phazite::getConnections (mix.context);
    using Kind = phazite::Connection::Kind;
    const auto has = [&connections] (const phazite::Connection& c)
    { return std::find (connections.begin(), connections.end(), c) != connections.end(); };

    CHECK (has ({ Kind::output, mix.id (mix.kick), mix.id (mix.drums), {} }));
    CHECK (has ({ Kind::output, mix.id (mix.drums), {}, {} }));
    CHECK (has ({ Kind::output, mix.id (mix.bass), {}, {} }));
    CHECK (has ({ Kind::output, mix.id (mix.reverb), {}, {} }));
    CHECK (has ({ Kind::send, mix.id (mix.keys), mix.id (mix.reverb), send->id }));
    CHECK (has ({ Kind::sidechain, mix.id (mix.kick), mix.id (mix.bass), compressor }));

    phazite::setSidechainSource (mix.context, compressor, {});
    CHECK_FALSE (phazite::getSidechainSource (mix.context, compressor).isValid());
    CHECK (mix.describeUndo() == juce::String ("Undo Turn Sidechain Off"));

    // How much CPU a channel's devices take: nothing while nothing plays, and never more than all of it.
    for (auto* track : { (te::Track*) mix.kick, (te::Track*) mix.bass, (te::Track*) mix.drums })
    {
        const auto load = phazite::getCpuLoad (mix.context, mix.id (track));
        CHECK (load >= 0.0f);
        CHECK (load <= 1.0f);
    }

    CHECK (phazite::getCpuLoad (mix.context, {}) == doctest::Approx (0.0f));
}

TEST_CASE ("A kick-to-bass sidechain shows as a cable in the routing view, and dragging cables routes, sends and listens")
{
    Mix mix;
    phazite::ui::RoutingView routing (mix.context);
    phazite::test::TestWindow window (routing, 1200, 400);
    phazite::test::dispatchPendingMessages();

    // A box for every channel, and the master.
    for (auto* track : { (te::Track*) mix.kick, (te::Track*) mix.bass, (te::Track*) mix.drums, (te::Track*) mix.reverb })
        CHECK_FALSE (routing.getBoxArea (mix.id (track)).isEmpty());

    CHECK_FALSE (routing.getBoxArea ({}).isEmpty());
    const auto compressor = phazite::getDeviceID (*mix.compressor);
    REQUIRE (routing.getSidechainJack (compressor).has_value());

    // The sidechain, as the compressor's picker sets it, shows while the bass is selected.
    phazite::setSidechainSource (mix.context, compressor, mix.id (mix.kick));
    mix.context.getSongSelection().selectOnly (mix.id (mix.bass));
    phazite::test::dispatchPendingMessages();

    const auto shown = routing.getShownConnections();
    const phazite::Connection cable { phazite::Connection::Kind::sidechain, mix.id (mix.kick), mix.id (mix.bass), compressor };
    CHECK (std::find (shown.begin(), shown.end(), cable) != shown.end());

    // A channel not selected shows none of its wires: Keys has only its output.
    for (const auto& connection : shown)
        CHECK ((connection.from == mix.id (mix.bass) || connection.to == mix.id (mix.bass)));

    // Dragging from Keys' Out jack: onto a return sends to it, onto a bus routes it there, onto a sidechain jack listens.
    phazite::test::Mouse mouse (routing);
    const auto dragCable = [&mouse] (juce::Point<int> from, juce::Point<int> to)
    {
        mouse.press (from);
        mouse.dragTo ((from + to) / 2);
        mouse.dragTo (to);
        mouse.release();
    };

    REQUIRE (routing.getOutJack (mix.id (mix.keys)).has_value());
    dragCable (*routing.getOutJack (mix.id (mix.keys)), routing.getBoxArea (mix.id (mix.reverb)).getCentre());
    CHECK (phazite::getSends (*mix.keys).size() == 1);

    dragCable (*routing.getOutJack (mix.id (mix.keys)), routing.getBoxArea (mix.id (mix.drums)).getCentre());
    CHECK (mix.keys->getParentFolderTrack() == mix.drums);

    phazite::test::dispatchPendingMessages();
    dragCable (*routing.getOutJack (mix.id (mix.keys)), *routing.getSidechainJack (compressor));
    CHECK (phazite::getSidechainSource (mix.context, compressor) == mix.id (mix.keys));

    // A right-click on a box offers to take its wires away.
    const auto menu = routing.getMenuFor (mix.id (mix.keys));
    auto removed = false;

    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        if (it.getItem().text.startsWith ("Remove the Send from Keys") && it.getItem().action != nullptr)
        {
            it.getItem().action();
            removed = true;
        }
    }

    CHECK (removed);
    CHECK (phazite::getSends (*mix.keys).isEmpty());
}

TEST_CASE ("A strip has a send knob for every return, and one drag sends the track to a reverb return")
{
    Mix mix;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::ChannelStrip strip { commands, mix.context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 800 };
    strip.setTrack (mix.id (mix.keys));
    phazite::test::dispatchPendingMessages();

    auto* reverbKnob = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (strip, "Send to Reverb"));
    auto* delayKnob = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (strip, "Send to Delay"));
    REQUIRE (reverbKnob != nullptr);
    REQUIRE (delayKnob != nullptr);
    CHECK (reverbKnob->isEnabled());
    CHECK (reverbKnob->getValue() == doctest::Approx (0.0));

    // One drag up: a send, at the level dragged to, one undo step.
    phazite::test::Mouse mouse (*reverbKnob);
    const auto centre = reverbKnob->getLocalBounds().getCentre();
    mouse.press (centre);
    mouse.dragTo (centre - juce::Point<int> (0, 20));
    mouse.dragTo (centre - juce::Point<int> (0, 40));
    mouse.release();

    REQUIRE (phazite::getSends (*mix.keys).size() == 1);
    CHECK (phazite::getSends (*mix.keys)[0]->getBusNumber() == phazite::getReturnBus (*mix.reverb));
    CHECK (phazite::getSends (*mix.keys)[0]->gain->getCurrentValue() > 0.1f);
    CHECK (mix.describeUndo() == juce::String ("Undo Send to Reverb"));
    CHECK (phazite::getSends (*mix.keys).size() == 1); // Delay's knob made nothing

    // The output button says where the sound goes, and follows a change of it.
    auto* output = dynamic_cast<juce::TextButton*> (phazite::test::findComponentWithTitle (strip, "Output"));
    REQUIRE (output != nullptr);
    CHECK (output->getButtonText() == juce::String ("Out: Master"));
    CHECK (output->isEnabled());

    phazite::setOutput (mix.context, mix.id (mix.keys), mix.id (mix.drums));
    CHECK (phazite::test::dispatchMessagesUntil ([output] { return output->getButtonText() == juce::String ("Out: Drums"); }));

    // A CPU bar along its bottom.
    auto* cpu = phazite::test::findComponentWithTitle (strip, "CPU");
    REQUIRE (cpu != nullptr);
    CHECK (cpu->isVisible());
}
