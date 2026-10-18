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
    render = nullptr;

    if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (0); // which deletes it
}

void RenderPanel::timerCallback()
{
    if (render != nullptr)
        progress = render->getProgress();
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
    cancelButton.setBounds (area.removeFromTop (controlHeight).removeFromRight (buttonWidth));
}
} // namespace phazite::ui
