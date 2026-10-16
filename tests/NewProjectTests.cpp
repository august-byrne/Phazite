// SPDX-License-Identifier: AGPL-3.0-or-later
// The new-project window (roadmap step 1.9, src/ui/shell/NewProjectPanel.h): the tempo, time signature, key, song outline
// and starting tracks it shows are what it asks the app to make the project with. Its name comes with its first save.
#include "UiTestHelpers.h"
#include "model/arrangement/Outlines.h"
#include "ui/shell/NewProjectPanel.h"

namespace
{
struct NewProjectWindow
{
    NewProjectWindow()
    {
        panel.onClose = [this] { closed = true; };
    }

    juce::Array<phazite::NewProjectSettings> settings; // what the window asked to make
    juce::Result answer = juce::Result::ok();          // what making it gives back
    bool closed = false;
    phazite::ui::NewProjectPanel panel { [this] (const phazite::NewProjectSettings& chosen)
                                         {
                                             settings.add (chosen);
                                             return answer;
                                         } };
    phazite::test::TestWindow window { panel, 540, 460 };

    void click (juce::Component& c) { phazite::test::Mouse (c).click (c.getLocalBounds().getCentre()); }
};

int getOutlineItemID (const juce::String& id)
{
    const auto& outlines = phazite::getBuiltInOutlines();

    for (size_t i = 0; i < outlines.size(); ++i)
        if (outlines[i].id == id)
            return (int) i + 2; // after "None"

    return 0;
}
} // namespace

TEST_CASE ("The new-project window makes a project with the choices it shows")
{
    NewProjectWindow w;
    phazite::test::checkChildrenApart (w.panel);

    // Its defaults: 120 BPM in 4/4, no key, no outline, and the two instrument tracks.
    const auto defaults = w.panel.getSettings();
    CHECK (defaults.tempo == doctest::Approx (120.0));
    CHECK (defaults.timeSignature == phazite::TimeSignature {});
    CHECK_FALSE (defaults.keyRoot.has_value());
    CHECK (defaults.outline.isEmpty());
    CHECK (defaults.withInstruments);
    CHECK (w.panel.getTempoField().getTextFromValue (120.0) == "120 BPM");
    CHECK_FALSE (w.panel.getScaleBox().isEnabled());
    CHECK (w.panel.getOutlineText().startsWith ("The song starts without sections."));

    w.panel.getTempoField().setValue (90.0, juce::sendNotificationSync);
    w.panel.getTimeSignatureBox().setSelectedId (5, juce::sendNotificationSync); // 2/4, 3/4, 4/4, 5/4, 6/8
    w.panel.getKeyBox().setSelectedId (4, juce::sendNotificationSync);           // None, C, C#, D
    CHECK (w.panel.getScaleBox().isEnabled());
    w.panel.getScaleBox().setSelectedId (2, juce::sendNotificationSync); // minor
    w.panel.getOutlineBox().setSelectedId (getOutlineItemID ("hipHop"), juce::sendNotificationSync);
    w.click (w.panel.getInstrumentsButton()); // off
    phazite::test::dispatchPendingMessages();

    const auto* hipHop = phazite::findOutline ("hipHop");
    REQUIRE (hipHop != nullptr);
    CHECK (w.panel.getOutlineText().startsWith (hipHop->sections.front().name + " " + juce::String (hipHop->sections.front().bars) + ", "));
    CHECK (w.panel.getOutlineText().endsWith ("(" + juce::String (hipHop->getLengthInBars()) + " bars)"));

    w.click (w.panel.getCreateButton());
    REQUIRE (w.settings.size() == 1);
    const auto& made = w.settings.getReference (0);
    CHECK (made.tempo == doctest::Approx (90.0));
    CHECK (made.timeSignature == phazite::TimeSignature { 6, 8 });
    CHECK (made.keyRoot == std::optional<int> (2));
    CHECK (made.scale == "minor");
    CHECK (made.outline == "hipHop");
    CHECK_FALSE (made.withInstruments);
    CHECK (phazite::isValid (made));
    CHECK (w.closed);
}

TEST_CASE ("The new-project window says why a project couldn't be made, and Enter makes it")
{
    NewProjectWindow w;

    w.answer = juce::Result::fail ("Phazite couldn't make a folder for a new project.");
    w.click (w.panel.getCreateButton());
    CHECK (w.settings.size() == 1);
    CHECK (w.panel.getError() == "Phazite couldn't make a folder for a new project.");
    CHECK_FALSE (w.closed);

    w.answer = juce::Result::ok();
    w.panel.grabKeyboardFocus(); // as it does once its window is on the screen
    w.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (w.settings.size() == 2);
    CHECK (w.closed);
}

TEST_CASE ("The new-project window's Cancel makes nothing")
{
    NewProjectWindow w;
    w.click (w.panel.getCancelButton());
    CHECK (w.closed);
    CHECK (w.settings.isEmpty());
}
