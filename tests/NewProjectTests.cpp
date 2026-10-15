// SPDX-License-Identifier: AGPL-3.0-or-later
// The new-project window (roadmap step 1.9, src/ui/shell/NewProjectPanel.h): the name, tempo, time signature, key, song
// outline and starting tracks it shows are what it asks the app to make the project with.
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

    juce::StringArray names; // what the window asked to make
    juce::Array<phazite::NewProjectSettings> settings;
    juce::Result answer = juce::Result::ok(); // what making it gives back
    bool closed = false;
    phazite::ui::NewProjectPanel panel { "Untitled", juce::File::getSpecialLocation (juce::File::tempDirectory),
                                         [this] (const juce::String& name, const phazite::NewProjectSettings& chosen)
                                         {
                                             names.add (name);
                                             settings.add (chosen);
                                             return answer;
                                         } };
    phazite::test::TestWindow window { panel, 540, 500 };

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
    CHECK (w.panel.getProjectName() == "Untitled");

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

    w.panel.getNameEditor().setText ("Beat", true);
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
    CHECK (w.names == juce::StringArray { "Beat" });
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

TEST_CASE ("The new-project window needs a name, and says why a project couldn't be made")
{
    NewProjectWindow w;

    w.panel.getNameEditor().setText ("  ", true);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (w.panel.getCreateButton().isEnabled());
    w.panel.getNameEditor().onReturnKey();
    CHECK (w.panel.getError() == "The project needs a name.");
    CHECK (w.settings.isEmpty());

    w.panel.getNameEditor().setText ("Beat", true);
    phazite::test::dispatchPendingMessages();
    CHECK (w.panel.getCreateButton().isEnabled());
    CHECK (w.panel.getError().isEmpty());

    w.answer = juce::Result::fail ("There is already a folder called \"Beat\" here.");
    w.click (w.panel.getCreateButton());
    CHECK (w.settings.size() == 1);
    CHECK (w.panel.getError() == "There is already a folder called \"Beat\" here.");
    CHECK_FALSE (w.closed);

    w.click (w.panel.getCancelButton());
    CHECK (w.closed);
    CHECK (w.settings.size() == 1);
}
