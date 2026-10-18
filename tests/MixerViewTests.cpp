// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer, as it lays out in the window (the inspector: InspectorTests.cpp).
#include "UiTestHelpers.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/MasterStrip.h"
#include "ui/mixer/MixerView.h"
#include "ui/theme/DesignTokens.h"

TEST_CASE ("The mixer's strips keep their parts apart, however short the window is")
{
    // A short window used to squeeze the faders over the devices and the names.
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->ensureNumberOfAudioTracks (2);
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setEdit (context, edit.get());
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 900, 600 };
    phazite::test::dispatchPendingMessages();

    for (const auto height : { 600, 320, 160, 60 })
    {
        CAPTURE (height);
        window.setSize (900, height);

        const auto strips = phazite::test::findAll<phazite::ui::ChannelStrip> (mixer);
        CHECK (strips.size() == 2);

        for (auto* strip : strips)
        {
            CHECK (strip->getHeight() >= phazite::ui::ChannelStrip::getMinimumHeight());
            phazite::test::checkChildrenApart (*strip);
        }

        for (auto* master : phazite::test::findAll<phazite::ui::MasterStrip> (mixer))
        {
            CHECK (master->getHeight() >= phazite::ui::MasterStrip::getMinimumHeight());
            phazite::test::checkChildrenApart (*master);
        }
    }

    setEdit (context, nullptr);
}

TEST_CASE ("A strip shows a track's instrument apart from its effects, and asks for one when its notes make no sound")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setEdit (context, edit.get());
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::ChannelStrip strip { commands, context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 700 };
    strip.setTrack (phazite::getTrackID (*track));

    // The texts of the buttons that show in a group of slots, as the user sees them.
    const auto buttonsIn = [&strip] (const juce::String& title)
    {
        juce::StringArray texts;

        if (auto* group = phazite::test::findComponentWithTitle (strip, title))
            for (auto* button : phazite::test::findAll<juce::TextButton> (*group))
                if (button->isVisible())
                    texts.add (button->getButtonText());

        return texts;
    };

    CHECK (buttonsIn ("Instrument") == juce::StringArray { "+ Instrument" });
    CHECK (buttonsIn ("Effects") == juce::StringArray { "+ Effect" });

    // Notes, and nothing to play them: the strip asks for an instrument.
    track->insertMIDIClip ({ te::TimePosition(), te::TimePosition::fromSeconds (2.0) }, nullptr);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return buttonsIn ("Instrument") == juce::StringArray { "Choose instrument" }; }));

    // The instrument has its own slot, and effects go in theirs.
    auto* synth = phazite::setInstrument (*track, edit->getPluginCache().createNewPlugin (phazite::devices::Synth::typeID, {}));
    auto* reverb = phazite::insertDevice (*track, edit->getPluginCache().createNewPlugin (phazite::devices::Reverb::typeID, {}));
    REQUIRE (synth != nullptr);
    REQUIRE (reverb != nullptr);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return buttonsIn ("Instrument").contains (synth->getName()); }));
    CHECK_FALSE (buttonsIn ("Instrument").contains ("+ Instrument"));
    CHECK_FALSE (buttonsIn ("Instrument").contains (reverb->getName()));
    CHECK (buttonsIn ("Effects").contains (reverb->getName()));
    CHECK (buttonsIn ("Effects").contains ("+ Effect"));
    phazite::test::checkChildrenApart (strip);
}

TEST_CASE ("The mixer puts the returns next to the master, and outlines what the selected channel sends to")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (*edit);
    auto* reverbReturn = phazite::addReturnTrack (*edit, "Reverb", nullptr);
    REQUIRE (reverbReturn != nullptr);
    edit->moveTrack (reverbReturn, { nullptr, nullptr }); // first in the playlist

    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setEdit (context, edit.get());
    auto& selection = getSelection (context);
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 900, 600 };
    phazite::test::dispatchPendingMessages();

    // The return's strip comes last, set apart from the tracks'.
    const auto strips = phazite::test::findAll<phazite::ui::ChannelStrip> (mixer);
    REQUIRE (strips.size() == 3);
    CHECK (strips[0]->getTrack() == phazite::getTrackID (*tracks[0]));
    CHECK (strips[2]->getTrack() == phazite::getTrackID (*reverbReturn));
    CHECK (strips[2]->getX() > strips[1]->getRight());

    // Selecting a track that sends to the return outlines the return's strip, and the other way round.
    phazite::addSend (*tracks[0], phazite::getReturnBus (*reverbReturn), false);
    selection.selectOnly (tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (strips[2]->isRelated());
    CHECK_FALSE (strips[1]->isRelated());

    selection.selectOnly (reverbReturn);
    phazite::test::dispatchPendingMessages();
    CHECK (strips[0]->isRelated());
    CHECK_FALSE (strips[1]->isRelated());

    selection.deselectAll();
    setEdit (context, nullptr);
}
