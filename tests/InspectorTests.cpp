// SPDX-License-Identifier: AGPL-3.0-or-later
// The inspector (src/ui/mixer/Inspector.h; roadmap step 2.5), driven as the user would: the properties of what is
// selected (a track, clips, a section), and the song's with nothing selected; changed from its rows, each one undo step.
#include "UiTestHelpers.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/SongSelection.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/Inspector.h"

#include <algorithm>

namespace
{
using phazite::BeatPosition;
using phazite::TrackID;

// An open project with two tracks, and the inspector in a window.
struct Screen
{
    Screen()
    {
        edit->ensureNumberOfAudioTracks (2);
        setEdit (context, edit.get());
        edit->getUndoManager().clearUndoHistory();
        phazite::test::dispatchPendingMessages();
    }

    ~Screen() { setEdit (context, nullptr); }

    TrackID track (int index) const { return phazite::getTrackID (*te::getAudioTracks (*edit)[index]); }

    // A label's or a button's text anywhere in the inspector.
    bool shows (const juce::String& text)
    {
        const auto labels = phazite::test::findAll<juce::Label> (inspector);
        const auto buttons = phazite::test::findAll<juce::TextButton> (inspector);
        return std::any_of (labels.begin(), labels.end(), [&text] (juce::Label* label) { return label->getText() == text; })
               || std::any_of (buttons.begin(), buttons.end(),
                               [&text] (juce::TextButton* button) { return button->getButtonText() == text; });
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Inspector inspector { commands, context };
    phazite::test::TestWindow window { inspector, 300, 900 };
};
} // namespace

TEST_CASE ("With nothing selected the inspector shows the song; a selected track shows its properties, which change it")
{
    Screen screen;
    auto& selection = screen.context.getSongSelection();
    selection.clear();
    phazite::test::dispatchPendingMessages();
    CHECK (screen.inspector.getShown() == juce::String ("Song"));
    CHECK (screen.shows ("120 BPM"));
    CHECK (screen.shows ("4/4"));

    selection.selectOnly (screen.track (1));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Track"));
    CHECK (screen.shows ("Track"));
    CHECK (screen.shows ("Master"));                                                       // where its sound goes
    CHECK (phazite::test::findComponentWithTitle (screen.inspector, "Volume") != nullptr); // its fader
    CHECK (phazite::test::findComponentWithTitle (screen.inspector, "Automation mode") != nullptr);
    phazite::test::checkChildrenApart (screen.inspector);

    // Typing a name and Enter renames it, as one undo step.
    auto editors = phazite::test::findAll<juce::TextEditor> (screen.inspector);
    REQUIRE (editors.size() == 1);
    editors.getFirst()->setText ("Bass", juce::dontSendNotification);
    REQUIRE (editors.getFirst()->onReturnKey != nullptr);
    editors.getFirst()->onReturnKey();
    CHECK (phazite::getTrackRow (screen.context, screen.track (1))->name == juce::String ("Bass"));
    CHECK (phazite::describeUndo (screen.edit.get()).isNotEmpty());

    // The volume fader is the track's.
    auto* volume = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (screen.inspector, "Volume"));
    REQUIRE (volume != nullptr);
    volume->setValue (0.2, juce::sendNotificationSync);
    CHECK (phazite::getVolume (screen.context, screen.track (1)).getValue() == doctest::Approx (0.2f).epsilon (0.01));
}

TEST_CASE ("A selected clip shows its place, length and whether it plays; a section shows its own properties")
{
    Screen screen;

    // A bar-long MIDI clip at the second bar (beat 4 in 4/4), selected as it is made.
    const auto clip = phazite::addMidiClipAt (screen.context, screen.track (0), BeatPosition::fromBeats (4.0));
    REQUIRE (clip.isValid());
    screen.context.getSongSelection().selectOnly (clip);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Clip"));
    CHECK (screen.shows ("Bar 2"));
    CHECK (screen.shows ("1 bar"));
    CHECK (screen.shows ("MIDI"));
    CHECK (screen.shows ("No"));                                                                       // not an alias
    CHECK_FALSE (phazite::test::findComponentWithTitle (screen.inspector, "Fade shape")->isVisible()); // MIDI clips don't fade

    // Its mute switch.
    auto* muted = phazite::test::findComponentWithTitle (screen.inspector, "Muted");
    REQUIRE (muted != nullptr);
    phazite::test::Mouse mouse (*muted);
    mouse.click (muted->getLocalBounds().getCentre());
    CHECK (phazite::getClip (screen.context, clip)->muted);
    phazite::test::checkChildrenApart (screen.inspector);

    // A section: its own page.
    const auto section = phazite::addSectionAt (screen.context, BeatPosition::fromBeats (8.0));
    REQUIRE (section.isValid());
    screen.context.getSongSelection().selectOnly (section);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.inspector.getShown() == juce::String ("Section"));
    CHECK (screen.shows ("Bar 3"));
}

TEST_CASE ("The inspector fills its panel, and scrolls up and down when the panel is shorter than what it shows")
{
    Screen screen;
    screen.context.getSongSelection().selectOnly (screen.track (0));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.inspector.getShown() == juce::String ("Track"));

    auto* viewport = phazite::test::findAll<juce::Viewport> (screen.inspector).getFirst();
    REQUIRE (viewport != nullptr);
    auto* page = viewport->getViewedComponent();
    REQUIRE (page != nullptr);
    CHECK (page->getWidth() >= 300 - viewport->getScrollBarThickness());
    CHECK (page->getHeight() >= 900);

    screen.window.setSize (300, 200);
    CHECK (page->getHeight() > 200); // it scrolls
}
