// SPDX-License-Identifier: AGPL-3.0-or-later
// Mixer depth (roadmap step 2.9; src/model/mixer/MixerModel.h): a send knob for every return, which its first turn makes;
// where a channel's sound goes, the master or a bus; a device's sidechain by its ID, as its menu picks it; each channel's
// CPU load; what links the channels; and a strip's send knobs and output button.
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/mixer/ChannelStrip.h"

namespace
{
// An open project with Kick, Bass and Keys, a bus folder Drums around Kick, and two returns, Reverb and Delay.
struct Mix
{
    Mix()
    {
        auto& s = *song;
        const auto tracks = phazite::ensureTracks (s, 3);
        kick = tracks[0];
        bass = tracks[1];
        keys = tracks[2];
        phazite::setTrackName (s, kick, "Kick");
        phazite::setTrackName (s, bass, "Bass");
        phazite::setTrackName (s, keys, "Keys");

        drums = phazite::groupIntoFolder (s, { kick }, nullptr);
        REQUIRE (drums.isValid());
        phazite::setTrackName (s, drums, "Drums");

        reverb = phazite::addReturnTrack (s, "Reverb", nullptr);
        delay = phazite::addReturnTrack (s, "Delay", nullptr);
        REQUIRE (reverb.isValid());
        REQUIRE (delay.isValid());

        // A compressor on the bass, which can listen to the kick.
        compressor = addCompressor (bass);

        s.getUndoManager().clearUndoHistory();
        setSong (context, song.get());
    }

    ~Mix() { setSong (context, nullptr); }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::DeviceID addCompressor (phazite::TrackID track)
    {
        const auto added =
            phazite::insertDeviceState (*song, track, phazite::makeOwnDeviceState (*song, phazite::devices::Compressor::typeID));
        REQUIRE (added.isValid());
        return added;
    }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::TrackID kick, bass, keys, reverb, delay, drums;
    phazite::DeviceID compressor;
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
    const auto targets = phazite::getSendTargets (mix.context, mix.keys);
    REQUIRE (targets.size() == 2);
    CHECK (targets[0].name == juce::String ("Reverb"));
    CHECK (targets[1].name == juce::String ("Delay"));
    CHECK_FALSE (targets[0].send.has_value());

    auto level = phazite::getSendLevelTo (mix.context, mix.keys, mix.reverb);
    CHECK (level.isValid());
    CHECK_FALSE (level.exists());
    CHECK (level.getValue() == doctest::Approx (0.0f));
    CHECK (level.getRange().end == doctest::Approx (1.0f));

    // A press without a drag makes nothing.
    level.beginGesture();
    level.endGesture();
    CHECK (phazite::getSends (*mix.song, mix.keys).empty());
    CHECK_FALSE (phazite::canUndo (*mix.song));

    // The first turn makes the send, after the fader, at the level turned to: one step.
    level.beginGesture();
    level.setValue (0.3f);
    level.setValue (0.6f);
    level.endGesture();
    REQUIRE (phazite::getSends (*mix.song, mix.keys).size() == 1);
    CHECK (level.exists());
    CHECK (level.getValue() == doctest::Approx (0.6f));
    CHECK_FALSE (phazite::isPreFader (*mix.song, phazite::getSends (*mix.song, mix.keys)[0]));
    CHECK (mix.describeUndo() == juce::String ("Undo Send to Reverb"));

    const auto withSend = phazite::getSendTargets (mix.context, mix.keys);
    REQUIRE (withSend[0].send.has_value());
    CHECK (withSend[0].send->returnName == juce::String ("Reverb"));

    // Later turns change its level.
    level.setValue (0.4f);
    CHECK (mix.describeUndo() == juce::String ("Undo Change Send Level"));

    phazite::undo (*mix.song);
    phazite::undo (*mix.song);
    CHECK (phazite::getSends (*mix.song, mix.keys).empty());
    CHECK_FALSE (level.exists());
    CHECK (level.isValid()); // a turn makes it again

    // A value set outside a drag (a key press) makes it too.
    level.setValue (0.5f);
    CHECK (phazite::getSends (*mix.song, mix.keys).size() == 1);
    CHECK (mix.describeUndo() == juce::String ("Undo Send to Reverb"));

    // A return sends to the others, not to itself, nor to one whose sound comes back to it; folders don't send.
    phazite::getSendLevelTo (mix.context, mix.delay, mix.reverb).setValue (0.5f);
    const auto fromReverb = phazite::getSendTargets (mix.context, mix.reverb);
    CHECK (fromReverb.empty());
    CHECK_FALSE (phazite::getSendLevelTo (mix.context, mix.reverb, mix.delay).isValid());
    CHECK (phazite::getSendTargets (mix.context, mix.drums).empty());
}

TEST_CASE ("Where a channel's sound goes: the master, or a bus, which the track goes into as a folder")
{
    Mix mix;
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.bass)) == juce::StringArray { "Master*", "Drums" });
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.kick)) == juce::StringArray { "Master", "Drums*" });
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.drums)) == juce::StringArray { "Master*" });
    CHECK (phazite::getOutputChoices (mix.context, mix.reverb).empty());
    CHECK_FALSE (phazite::canChooseOutput (mix.context, mix.reverb));
    CHECK (phazite::canChooseOutput (mix.context, mix.bass));

    phazite::setOutput (mix.context, mix.bass, mix.drums);
    CHECK (phazite::getParentFolder (*mix.song, mix.bass) == mix.drums);
    CHECK (mix.describeUndo() == juce::String ("Undo Change Output"));
    CHECK (phazite::getChannel (mix.context, mix.bass)->outputBus == juce::String ("Drums"));

    phazite::setOutput (mix.context, mix.bass, {});
    CHECK_FALSE (phazite::getParentFolder (*mix.song, mix.bass).isValid());
    CHECK (phazite::getChannel (mix.context, mix.bass)->outputBus.isEmpty());

    // Choosing what is chosen already, or a plain folder, changes nothing.
    phazite::setOutput (mix.context, mix.bass, {});
    CHECK (mix.describeUndo() == juce::String ("Undo Change Output"));
    phazite::undo (*mix.song);
    phazite::undo (*mix.song);
    CHECK_FALSE (phazite::getParentFolder (*mix.song, mix.bass).isValid());

    phazite::setMixedAsBus (mix.context, mix.drums, false);
    CHECK (namesOf (phazite::getOutputChoices (mix.context, mix.bass)) == juce::StringArray { "Master*" });
    phazite::setOutput (mix.context, mix.bass, mix.drums);
    CHECK_FALSE (phazite::getParentFolder (*mix.song, mix.bass).isValid());
}

TEST_CASE ("A device's sidechain by its ID, and what links the channels: outputs, sends and sidechains")
{
    Mix mix;
    const auto compressor = mix.compressor;
    CHECK (phazite::hasSidechain (mix.context, compressor));
    CHECK_FALSE (phazite::getSidechainSource (mix.context, compressor).isValid());

    const auto choices = phazite::getSidechainChoices (mix.context, compressor);
    CHECK (std::any_of (choices.begin(), choices.end(), [&mix] (const auto& c) { return c.track == mix.kick; }));
    CHECK (std::none_of (choices.begin(), choices.end(), [&mix] (const auto& c) { return c.track == mix.bass; }));

    phazite::setSidechainSource (mix.context, compressor, mix.kick);
    CHECK (phazite::getSidechainSource (mix.context, compressor) == mix.kick);
    CHECK (mix.describeUndo() == juce::String ("Undo Sidechain from Kick"));

    phazite::getSendLevelTo (mix.context, mix.keys, mix.reverb).setValue (0.5f);
    const auto send = phazite::getSendTargets (mix.context, mix.keys)[0].send;
    REQUIRE (send.has_value());

    const auto connections = phazite::getConnections (mix.context);
    using Kind = phazite::Connection::Kind;
    const auto has = [&connections] (const phazite::Connection& c)
    { return std::find (connections.begin(), connections.end(), c) != connections.end(); };

    CHECK (has ({ Kind::output, mix.kick, mix.drums, {} }));
    CHECK (has ({ Kind::output, mix.drums, {}, {} }));
    CHECK (has ({ Kind::output, mix.bass, {}, {} }));
    CHECK (has ({ Kind::output, mix.reverb, {}, {} }));
    CHECK (has ({ Kind::send, mix.keys, mix.reverb, send->id }));
    CHECK (has ({ Kind::sidechain, mix.kick, mix.bass, compressor }));

    phazite::setSidechainSource (mix.context, compressor, {});
    CHECK_FALSE (phazite::getSidechainSource (mix.context, compressor).isValid());
    CHECK (mix.describeUndo() == juce::String ("Undo Turn Sidechain Off"));

    // How much CPU a channel's devices take: nothing while nothing plays, and never more than all of it.
    for (const auto track : { mix.kick, mix.bass, mix.drums })
    {
        const auto load = phazite::getCpuLoad (mix.context, track);
        CHECK (load >= 0.0f);
        CHECK (load <= 1.0f);
    }

    CHECK (phazite::getCpuLoad (mix.context, {}) == doctest::Approx (0.0f));
}

TEST_CASE ("A strip has a send knob for every return, and one drag sends the track to a reverb return")
{
    Mix mix;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::ChannelStrip strip { commands, mix.context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 800 };
    strip.setTrack (mix.keys);
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

    REQUIRE (phazite::getSends (*mix.song, mix.keys).size() == 1);
    const auto send = phazite::getSends (*mix.song, mix.keys)[0];
    CHECK (phazite::getSendTarget (*mix.song, send) == mix.reverb);
    CHECK (phazite::getSendLevel (mix.context, send).getValue() > 0.1f); // the knob's position
    CHECK (mix.describeUndo() == juce::String ("Undo Send to Reverb"));
    CHECK (phazite::getSends (*mix.song, mix.keys).size() == 1); // Delay's knob made nothing

    // The output button says where the sound goes, and follows a change of it.
    auto* output = dynamic_cast<juce::TextButton*> (phazite::test::findComponentWithTitle (strip, "Output"));
    REQUIRE (output != nullptr);
    CHECK (output->getButtonText() == juce::String ("Out: Master"));
    CHECK (output->isEnabled());

    phazite::setOutput (mix.context, mix.keys, mix.drums);
    CHECK (phazite::test::dispatchMessagesUntil ([output] { return output->getButtonText() == juce::String ("Out: Drums"); }));

    // A CPU bar along its bottom.
    auto* cpu = phazite::test::findComponentWithTitle (strip, "CPU");
    REQUIRE (cpu != nullptr);
    CHECK (cpu->isVisible());
}

TEST_CASE ("The sidechain's menu lists Off and every other track, greys one that would make a loop with why, and picks one")
{
    Mix mix;
    const auto compressor = mix.compressor;

    // The keys' sound comes from the bass, through a compressor on the keys that listens to it.
    const auto onKeys = mix.addCompressor (mix.keys);
    phazite::setSidechainSource (mix.context, onKeys, mix.bass);

    const auto itemsOf = [] (juce::PopupMenu& menu)
    {
        std::vector<juce::PopupMenu::Item> items;

        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (! it.getItem().isSeparator)
                items.push_back (it.getItem());

        return items;
    };

    auto menu = phazite::ui::makeSidechainMenu (mix.context, compressor);
    auto items = itemsOf (menu);
    REQUIRE (items.size() == 5);
    CHECK (items[0].text == juce::String ("Off: the sound passing through it"));
    CHECK (items[0].isTicked);
    CHECK (items[1].text == juce::String ("Kick"));
    CHECK (items[2].text == juce::String ("Keys (its sound comes from Bass)"));
    CHECK_FALSE (items[2].isEnabled);
    CHECK (items[3].text == juce::String ("Reverb"));

    // Kick: one step, the compressor ducking, and its slot in the mixer shows where it listens.
    items[1].action();
    CHECK (mix.describeUndo() == juce::String ("Undo Sidechain from Kick"));
    CHECK (phazite::getValueSetByHand (*mix.song, { mix.compressor, "point2X" })
           == doctest::Approx (phazite::devices::curves::axisForDecibels (-30.0f))); // ducking, 8:1 over -30 dB
    const auto slots = phazite::getEffects (mix.context, mix.bass);
    REQUIRE (slots.size() == 1);
    CHECK (slots[0].hasSidechain);
    CHECK (slots[0].sidechain.name == juce::String ("Kick"));

    // Duck Under, from the track's menu: the keys can't (their sound comes from the bass); Kick adds a second compressor.
    auto duck = phazite::ui::makeDuckUnderMenu (mix.context, mix.bass);
    auto ducks = itemsOf (duck);
    REQUIRE (ducks.size() == 4);
    CHECK_FALSE (ducks[1].isEnabled);
    ducks[0].action();
    CHECK (mix.describeUndo() == juce::String ("Undo Duck Bass Under Kick"));
    CHECK (phazite::getEffects (mix.context, mix.bass).size() == 2);
}
