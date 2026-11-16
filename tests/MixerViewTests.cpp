// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer, as it lays out in the window (the inspector: InspectorTests.cpp).
#include "TestClips.h"
#include "UiTestHelpers.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/MasterStrip.h"
#include "ui/mixer/MixerView.h"
#include "ui/mixer/StripParts.h"
#include "ui/theme/DesignTokens.h"

#include <stdexcept>

TEST_CASE ("The mixer's strips keep their parts apart, however short the window is")
{
    // A short window used to squeeze the faders over the devices and the names.
    auto song = phazite::createSong();
    phazite::ensureTracks (*song, 2);
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setSong (context, song.get());
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

    setSong (context, nullptr);
}

TEST_CASE ("A strip shows a track's instrument apart from its effects, and asks for one when its notes make no sound")
{
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1)[0];
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setSong (context, song.get());
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::ChannelStrip strip { commands, context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 700 };
    strip.setTrack (track);

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
    phazite::test::addMidiClip (*song, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) });
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return buttonsIn ("Instrument") == juce::StringArray { "Choose instrument" }; }));

    // The instrument has its own slot, and effects go in theirs.
    const auto synth = phazite::setInstrument (*song, track, phazite::makeOwnDeviceState (*song, phazite::devices::Synth::typeID));
    const auto reverb = phazite::insertDeviceState (*song, track, phazite::makeOwnDeviceState (*song, phazite::devices::Reverb::typeID));
    REQUIRE (synth.isValid());
    REQUIRE (reverb.isValid());
    const auto synthName = phazite::getDeviceName (*song, synth);
    const auto reverbName = phazite::getDeviceName (*song, reverb);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return buttonsIn ("Instrument").contains (synthName); }));
    CHECK_FALSE (buttonsIn ("Instrument").contains ("+ Instrument"));
    CHECK_FALSE (buttonsIn ("Instrument").contains (reverbName));
    CHECK (buttonsIn ("Effects").contains (reverbName));
    CHECK (buttonsIn ("Effects").contains ("+ Effect"));
    phazite::test::checkChildrenApart (strip);
}

TEST_CASE ("A strip lists its devices a line each, with no knobs, over a fader as tall in every strip")
{
    // The owner (2026-10-30): the mixer showed knobs for effects' parameters, and a strip with many effects squeezed its
    // fader shorter than its neighbours'.
    auto song = phazite::createSong();
    const auto busy = phazite::ensureTracks (*song, 2)[0];

    for (int i = 0; i < 6; ++i)
        REQUIRE (phazite::insertDeviceState (*song, busy, phazite::makeOwnDeviceState (*song, phazite::devices::Reverb::typeID)).isValid());

    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, song.get());
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 900, 700 };
    phazite::test::dispatchPendingMessages();

    for (const auto height : { 700, 420 })
    {
        CAPTURE (height);
        window.setSize (900, height);
        const auto strips = phazite::test::findAll<phazite::ui::ChannelStrip> (mixer);
        REQUIRE (strips.size() == 2);

        const auto fader = [] (juce::Component& strip)
        {
            auto* found = phazite::test::findComponentWithTitle (strip, "Volume");
            REQUIRE (found != nullptr);
            return found->getHeight();
        };

        CHECK (fader (*strips[0]) == fader (*strips[1]));
        CHECK (fader (*strips[0]) == phazite::ui::strip::faderHeight);

        // The busy strip's six effects, each a line with its switch and name only; they scroll in what is left.
        auto* effects = phazite::test::findComponentWithTitle (*strips[0], "Effects");
        REQUIRE (effects != nullptr);
        CHECK (phazite::test::findAll<phazite::ui::Knob> (*effects).isEmpty());
        CHECK (effects->getHeight()
               <= 6 * (phazite::ui::strip::slotNameHeight + phazite::ui::Space::xs) + phazite::ui::strip::buttonHeight);
        phazite::test::checkChildrenApart (*strips[0]);
    }

    setSong (context, nullptr);
}

TEST_CASE ("The mixer puts the returns next to the master, and outlines what the selected channel sends to")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto reverbReturn = phazite::addReturnTrack (*song, "Reverb", nullptr);
    REQUIRE (reverbReturn.isValid());
    phazite::moveTracksAfter (*song, { reverbReturn }, {}); // first in the playlist

    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setSong (context, song.get());
    auto& selection = getSelection (context);
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 900, 600 };
    phazite::test::dispatchPendingMessages();

    // The return's strip comes last, set apart from the tracks'.
    const auto strips = phazite::test::findAll<phazite::ui::ChannelStrip> (mixer);
    REQUIRE (strips.size() == 3);
    CHECK (strips[0]->getTrack() == tracks[0]);
    CHECK (strips[2]->getTrack() == reverbReturn);
    CHECK (strips[2]->getX() > strips[1]->getRight());

    // Selecting a track that sends to the return outlines the return's strip, and the other way round.
    REQUIRE (phazite::addSend (*song, tracks[0], reverbReturn, false).isValid());
    selection.selectOnly (tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (strips[2]->isRelated());
    CHECK_FALSE (strips[1]->isRelated());

    selection.selectOnly (reverbReturn);
    phazite::test::dispatchPendingMessages();
    CHECK (strips[0]->isRelated());
    CHECK_FALSE (strips[1]->isRelated());

    selection.deselectAll();
    setSong (context, nullptr);
}

TEST_CASE ("The mixer has no row of its own: a slim column after the returns adds one, and a sidechain outlines both its ends")
{
    auto song = phazite::createSong();
    phazite::ensureTracks (*song, 2);
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    setSong (context, song.get());
    const auto tracks = phazite::getAllPlaylistTracks (context);
    REQUIRE (tracks.size() == 2);
    phazite::CommandRegistry registry;
    int returnsAdded = 0;
    registry.add ({ .id = "track.addReturn", .name = "Add Return Track", .perform = [&returnsAdded] { ++returnsAdded; } });
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 900, 600 };
    phazite::test::dispatchPendingMessages();

    // The strips start at the mixer's top, and the "+" sits after the last of them, level with their names.
    const auto strips = phazite::test::findAll<phazite::ui::ChannelStrip> (mixer);
    REQUIRE (strips.size() == 2);
    CHECK (strips[0]->getScreenY() == mixer.getScreenY());
    auto* add = phazite::test::findComponentWithTitle (mixer, "Add Return Track");
    REQUIRE (add != nullptr);
    CHECK (add->isShowing());
    CHECK (add->getScreenX() >= strips[1]->getScreenBounds().getRight());
    CHECK (add->getScreenY() < strips[1]->getScreenY() + phazite::ui::strip::headerHeight);
    phazite::test::Mouse mouse (*add);
    mouse.click (add->getLocalBounds().getCentre());
    CHECK (returnsAdded == 1);

    // A sidechain links its two ends: selecting the kick outlines the bass that ducks under it, and the other way round.
    phazite::duckUnder (context, tracks[1], tracks[0]);
    auto& selection = context.getSongSelection();
    selection.selectOnly (tracks[0]);
    phazite::test::dispatchPendingMessages();
    CHECK (strips[1]->isRelated());

    selection.selectOnly (tracks[1]);
    phazite::test::dispatchPendingMessages();
    CHECK (strips[0]->isRelated());

    selection.clear();
    setSong (context, nullptr);
}

namespace
{
// A mixer of many tracks in a window that shows a few of them, as the app has it.
struct BigMixer
{
    explicit BigMixer (int numTracks)
    {
        tracks = phazite::ensureTracks (*song, numTracks);
        song->getUndoManager().clearUndoHistory();
        setSong (context, song.get());
        phazite::test::dispatchPendingMessages();
        channels = dynamic_cast<juce::Viewport*> (phazite::test::findComponentWithTitle (mixer, "Channels"));
        REQUIRE (channels != nullptr);
    }

    ~BigMixer() { setSong (context, nullptr); }

    phazite::ui::ChannelStrip* stripOf (phazite::TrackID track)
    {
        for (auto* strip : phazite::test::findAll<phazite::ui::ChannelStrip> (mixer))
            if (strip->getTrack() == track)
                return strip;

        return nullptr;
    }

    phazite::Song::Ptr song = phazite::createSong();
    std::vector<phazite::TrackID> tracks;
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 900, 600 };
    juce::Viewport* channels = nullptr;
};
} // namespace

TEST_CASE ("The mixer makes strips as their channels scroll into view, and keeps the one whose fader is dragged while it scrolls away")
{
    BigMixer screen (40);

    // The channels in view and one more, not the last; a screen reader hears how many there are.
    CHECK (phazite::test::findAll<phazite::ui::ChannelStrip> (screen.mixer).size() < 12);
    REQUIRE (screen.stripOf (screen.tracks.front()) != nullptr);
    CHECK (screen.stripOf (screen.tracks.back()) == nullptr);
    CHECK (screen.channels->getDescription().contains ("of 40"));

    // The first track's fader dragged up, and the mixer scrolled to its end meanwhile, as the wheel would scroll it.
    juce::Component::SafePointer<phazite::ui::ChannelStrip> first (screen.stripOf (screen.tracks.front()));
    auto* fader = phazite::test::findComponentWithTitle (*first, "Volume");
    REQUIRE (fader != nullptr);
    const auto before = phazite::getVolume (screen.context, screen.tracks.front()).getValue();
    phazite::test::Mouse mouse (*fader);
    const auto centre = fader->getLocalBounds().getCentre();
    mouse.press (centre);
    mouse.dragTo (centre.translated (0, -20));
    screen.channels->setViewPosition (screen.channels->getViewedComponent()->getWidth(), 0);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.stripOf (screen.tracks.back()) != nullptr);
    CHECK (first != nullptr); // kept, with its fader, while it is dragged
    mouse.dragTo (centre.translated (0, -40));
    mouse.release();
    const auto after = phazite::getVolume (screen.context, screen.tracks.front()).getValue();
    CHECK (after > before);

    // One undo step, which puts it back.
    phazite::undo (*screen.song);
    CHECK (juce::exactlyEqual (phazite::getVolume (screen.context, screen.tracks.front()).getValue(), before));

    // Once it is let go, and the keys have gone elsewhere, its strip goes as the mixer scrolls; back in view, the channel has
    // a strip again, which shows it as it is now.
    REQUIRE (first != nullptr);
    screen.mixer.grabKeyboardFocus();
    screen.channels->setViewPosition (screen.channels->getViewPositionX() - 1, 0);
    CHECK (first == nullptr);
    screen.channels->setViewPosition (0, 0);
    phazite::test::dispatchPendingMessages();
    auto* again = screen.stripOf (screen.tracks.front());
    REQUIRE (again != nullptr);
    auto* name = phazite::test::findAll<juce::Label> (*again).getFirst();
    REQUIRE (name != nullptr);
    CHECK (name->getText() == phazite::getChannel (screen.context, screen.tracks.front())->name);
}

TEST_CASE ("The Tab key goes on from the last strip in view to the next channel's, which scrolls into view")
{
    BigMixer screen (40);
    const auto view = screen.channels->getViewArea();
    const auto lastWhole = view.getRight() / phazite::ui::ChannelStrip::width - 1; // no returns: no gap
    auto* last = screen.stripOf (screen.tracks[(size_t) lastWhole]);
    REQUIRE (last != nullptr);
    REQUIRE (view.contains (last->getBounds()));
    auto* fader = phazite::test::findComponentWithTitle (*last, "Volume");
    REQUIRE (fader != nullptr);
    fader->grabKeyboardFocus();

    const auto next = screen.tracks[(size_t) lastWhole + 1];
    const auto nextHasFocus = [&] { return screen.stripOf (next) != nullptr && screen.stripOf (next)->hasKeyboardFocus (true); };

    for (int i = 0; i < 60 && ! nextHasFocus(); ++i)
        screen.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));

    REQUIRE (nextHasFocus());
    phazite::test::dispatchPendingMessages(); // it scrolls into view as the focus has moved
    CHECK (screen.channels->getViewArea().contains (screen.stripOf (next)->getBounds()));
    CHECK (screen.stripOf (screen.tracks[(size_t) lastWhole + 2]) != nullptr); // the one after it, for the Tab key to go on to
}

TEST_CASE ("A send that would make a loop isn't made, whatever offers it")
{
    // Returns R1, R2 and R3, R1 sending to R3 and R2 to R1: R2's sound reaches R3, so a send from R3 to R2 would bring it back.
    auto song = phazite::createSong();
    const auto r1 = phazite::addReturnTrack (*song, "R1", nullptr);
    const auto r2 = phazite::addReturnTrack (*song, "R2", nullptr);
    const auto r3 = phazite::addReturnTrack (*song, "R3", nullptr);
    REQUIRE (phazite::addSend (*song, r1, r3, false).isValid());
    REQUIRE (phazite::addSend (*song, r2, r1, false).isValid());
    CHECK_FALSE (phazite::addSend (*song, r3, r2, false).isValid());
    CHECK_FALSE (phazite::addSend (*song, r3, r1, false).isValid());
    CHECK (phazite::getSends (*song, r3).empty());

    // Without the loop, it is made.
    const auto track = phazite::ensureTracks (*song, 1)[0];
    CHECK (phazite::addSend (*song, track, r2, false).isValid());
}

TEST_CASE ("When a send makes a loop of another's, the mixer takes that strip's knob away, and a knob left turning makes none")
{
    // Returns R1, R2 and R3, R1 sending to R3: R3's strip has knobs to send to R1 and R2.
    auto song = phazite::createSong();
    const auto r1 = phazite::addReturnTrack (*song, "R1", nullptr);
    const auto r2 = phazite::addReturnTrack (*song, "R2", nullptr);
    const auto r3 = phazite::addReturnTrack (*song, "R3", nullptr);
    REQUIRE (phazite::addSend (*song, r1, r3, false).isValid());
    song->getUndoManager().clearUndoHistory();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, song.get());
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 900, 700 };
    phazite::test::dispatchPendingMessages();

    const auto stripOf = [&mixer] (phazite::TrackID track) -> phazite::ui::ChannelStrip&
    {
        for (auto* strip : phazite::test::findAll<phazite::ui::ChannelStrip> (mixer))
            if (strip->getTrack() == track)
                return *strip;

        FAIL ("no strip shows the track");
        throw std::logic_error ("unreachable");
    };

    const auto hasKnob = [&] (phazite::TrackID on, const juce::String& to)
    { return phazite::test::findComponentWithTitle (stripOf (on), "Send to " + to) != nullptr; };

    REQUIRE (hasKnob (r3, "R2"));
    auto stale = phazite::getSendLevelTo (context, r3, r2); // as R3's knob holds it
    REQUIRE (stale.isValid());

    // R2's knob to R1 turned, which makes the send: R2's sound now reaches R3, so R3's knob to R2 goes.
    auto toR1 = phazite::getSendLevelTo (context, r2, r1);
    toR1.beginGesture();
    toR1.setValue (0.5f);
    toR1.endGesture();
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::getSends (*song, r2).size() == 1);
    CHECK (phazite::test::runTimersUntil ([&] { return ! stripOf (r3).isRefreshDue(); }));
    CHECK_FALSE (hasKnob (r3, "R2"));

    // A knob that was still there, turned, makes no send: the send would bring R2's sound back to it.
    stale.beginGesture();
    stale.setValue (0.5f);
    stale.endGesture();
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getSends (*song, r3).empty());

    setSong (context, nullptr);
}
