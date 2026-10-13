// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer and the inspector, as they lay out in the window.
#include "UiTestHelpers.h"
#include "ui/ChannelStrip.h"
#include "ui/Mixer.h"

TEST_CASE ("The mixer's strips keep their parts apart, however short the window is")
{
    // A short window used to squeeze the faders over the devices and the names.
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->ensureNumberOfAudioTracks (2);
    te::SelectionManager selection { phazite::test::getEngine() };
    selection.edit = edit.get();
    phazite::CommandRegistry registry;
    phazite::ui::Mixer mixer { phazite::test::getEngine(), registry, selection };
    phazite::test::TestWindow window { mixer, 900, 600 };
    mixer.setEdit (edit.get());
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

    mixer.setEdit (nullptr);
}
