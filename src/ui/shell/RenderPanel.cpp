// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RenderPanel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int buttonWidth = 96; // as the export window's
} // namespace

RenderPanel::RenderPanel (std::unique_ptr<TrackRender> r)
    : render (std::move (r))
{
    setTitle (render != nullptr ? render->getTitle() : juce::String());
    title.setText ((render != nullptr ? render->getTitle() : juce::String ("Rendering")) + "...", juce::dontSendNotification);
    title.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    progressBar.setTitle ("Progress");
    cancelButton.setTooltip ("Stops it, leaving the project as it was");
    cancelButton.onClick = [this] { cancel(); };

    for (auto* c : std::initializer_list<juce::Component*> { &title, &progressBar, &cancelButton })
        addAndMakeVisible (c);

    // A second way out, where it has one (a command's wait for the plug-ins' states: Go On Without Them, its Cancel then
    // dropping the command); and Escape is Cancel.
    if (const auto name = render != nullptr ? render->getGoOnName() : juce::String(); name.isNotEmpty())
    {
        goOnButton.setButtonText (name);
        goOnButton.setTooltip ("Goes on with the settings each plug-in gave last");
        goOnButton.onClick = [this] { goOn(); };
        addAndMakeVisible (goOnButton);
        cancelButton.setTooltip ("Drops what waits, changing nothing");
    }

    setWantsKeyboardFocus (true);

    setSize (width, height);
    startTimerHz (10);
}

RenderPanel::~RenderPanel()
{
    stopTimer();
    render = nullptr; // cancels a render that is still going
}

void RenderPanel::cancel()
{
    stopTimer();

    if (render != nullptr)
        render->cancel();

    render = nullptr;
    const auto cancelled = onCancelled; // this goes with its window

    if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (0); // which deletes it

    if (cancelled != nullptr)
        cancelled();
}

void RenderPanel::goOn()
{
    // What it does first, then the window goes as Cancel's does, the render finished.
    if (render != nullptr)
        render->goOn();

    cancel();
}

bool RenderPanel::keyPressed (const juce::KeyPress& key)
{
    if (key != juce::KeyPress::escapeKey || goOnButton.getButtonText().isEmpty())
        return false;

    cancel();
    return true;
}

void RenderPanel::timerCallback()
{
    if (render == nullptr)
        return;

    progress = render->getProgress();

    // A plug-in it has waited for as long as a hang takes live, named on the bar (known bug 42): Cancel is the user's.
    showWait (progressBar, shownWait, render->getWait());
}

void RenderPanel::showWait (juce::ProgressBar& bar, juce::String& shown, const std::optional<RenderWait>& wait)
{
    const auto text = wait.has_value() ? "Waiting for " + wait->plugin + " (" + juce::String ((int) wait->seconds) + " s)" : juce::String();

    if (text == shown)
        return;

    shown = text;

    // The percentage again once the wait is over (JUCE's bar shows no percentage from the time it is given a text).
    if (text.isEmpty())
        bar.setPercentageDisplay (true);
    else
        bar.setTextToDisplay (text);
}

void RenderPanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void RenderPanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);
    title.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::m);
    progressBar.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::m);
    auto buttons = area.removeFromTop (controlHeight);
    cancelButton.setBounds (buttons.removeFromRight (buttonWidth));
    buttons.removeFromRight (Space::m);
    goOnButton.setBounds (buttons.removeFromRight (2 * buttonWidth));
}
} // namespace phazite::ui
