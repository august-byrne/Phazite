// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdatePanel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int buttonWidth = 150; // as wide as "Download and Install" needs

    using State = Updates::State;

    juce::String describeSize (juce::int64 bytes)
    {
        return juce::File::descriptionOfSizeInBytes (bytes);
    }
} // namespace

juce::String getUpdateCommandName (const Updates& updates)
{
    if (const auto found = updates.getAvailable(); found.has_value() && isUpdateWaiting (updates))
        return "Update to " + juce::String (appName) + " " + found->version + "...";

    return "Check for Updates...";
}

bool isUpdateWaiting (const Updates& updates)
{
    const auto state = updates.getState();
    return updates.getAvailable().has_value() && (state == State::available || state == State::downloading || state == State::ready);
}

UpdatePanel::UpdatePanel (Updates& u, std::function<void()> installUpdate)
    : updates (u),
      install (std::move (installUpdate))
{
    setTitle ("Updates");
    title.setText ("Updates", juce::dontSendNotification);
    title.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    running.setText ("This is " + juce::String (appName) + " " + updates.getRunningVersion() + ".", juce::dontSendNotification);
    status.setJustificationType (juce::Justification::topLeft);
    status.setFont (juce::FontOptions (TextSize::body));
    privacy.setText ("Phazite then asks its releases on GitHub for one small file each time it opens, and sends nothing about you or your "
                     "songs. Otherwise it asks only when you open this window.",
                     juce::dontSendNotification);
    privacy.setJustificationType (juce::Justification::topLeft);
    privacy.setFont (juce::FontOptions (TextSize::small));

    action.onClick = [this] { runAction(); };
    release.onClick = [this]
    {
        const auto found = updates.getAvailable();
        (found.has_value() ? found->notes : updates.getSource().releasesPage).launchInDefaultBrowser();
    };

    checkAtStart.setTooltip ("Off until you turn it on: Phazite asks for nothing on the internet unless you do");
    checkAtStart.onClick = [this] { updates.setChecksAtStart (checkAtStart.getToggleState()); };

    for (auto* component :
         std::initializer_list<juce::Component*> { &title, &running, &status, &progressBar, &action, &release, &checkAtStart, &privacy })
        addAndMakeVisible (component);

    lookAndFeelChanged();
    updates.addChangeListener (this);
    refresh();
    setSize (width, height);
}

UpdatePanel::~UpdatePanel()
{
    updates.removeChangeListener (this);
}

void UpdatePanel::refresh()
{
    const auto state = updates.getState();
    const auto found = updates.getAvailable();
    const auto version = found.has_value() ? juce::String (appName) + " " + found->version : juce::String();
    const auto canInstall = found.has_value() && ! found->installer.isEmpty(); // Windows: elsewhere, the release's page
    juce::String said, step;

    switch (state)
    {
        case State::checking:
            said = "Asking Phazite's releases on GitHub for the latest version...";
            step = "Cancel";
            break;

        case State::upToDate:
        {
            const auto latest = updates.getLatestVersion();
            said = Updates::compareVersions (updates.getRunningVersion(), latest) > 0
                       ? "This Phazite is newer than its latest release, " + latest + "."
                       : juce::String (appName) + " " + updates.getRunningVersion() + " is the latest version.";
            step = "Check Again";
            break;
        }

        case State::available:
            said = canInstall ? version
                                    + " is out. Download and Install downloads its installer and checks that its SHA-256 is the "
                                      "release's; Phazite then closes, asking to save first, installs it and opens again."
                              : version + " is out: its release's page has the download.";
            step = canInstall ? "Download and Install" : "";
            break;

        case State::downloading:
        {
            const auto size = found.has_value() ? found->size : (juce::int64) 0;
            said = "Downloading " + version + ": " + describeSize ((juce::int64) (updates.getProgress() * (double) size)) + " of "
                   + describeSize (size) + ".";
            step = "Cancel";
            break;
        }

        case State::ready:
            said = version
                   + " is downloaded, and its SHA-256 is the release's. Install and Restart closes Phazite, asking to save "
                     "first, installs it and opens it again.";
            step = "Install and Restart";
            break;

        case State::failed:
            said = updates.getError();
            step = Updates::canCheck() ? "Try Again" : "";
            break;

        case State::idle:
        default:
            said = Updates::canCheck() ? "Phazite hasn't looked for a newer version yet."
                                       : "This build of Phazite can't check for updates: its releases page lists them.";
            step = Updates::canCheck() ? "Check Now" : "";
            break;
    }

    status.setText (said, juce::dontSendNotification);
    setThemeColour (status, juce::Label::textColourId, state == State::failed ? &Palette::warning : &Palette::text);
    progress = state == State::downloading ? updates.getProgress() : 0.0;
    progressBar.setVisible (state == State::downloading);
    action.setButtonText (step);
    action.setVisible (step.isNotEmpty());
    action.setTooltip (step == "Install and Restart" ? "Closes Phazite, asking to save first, and installs the update" : juce::String());
    release.setButtonText (found.has_value() ? "Release Notes" : "All Releases");
    release.setTooltip (found.has_value() ? "Opens the release's page in the browser"
                                          : "Opens the list of Phazite's releases in the browser");
    checkAtStart.setToggleState (updates.checksAtStart(), juce::dontSendNotification);
}

void UpdatePanel::runAction()
{
    switch (updates.getState())
    {
        case State::checking:
        case State::downloading:
            updates.cancel();
            break;

        case State::available:
            updates.download();
            break;

        case State::ready:
            if (install != nullptr)
                install();

            break;

        case State::failed:
            if (updates.getAvailable().has_value())
                updates.download();
            else
                updates.check();

            break;

        case State::idle:
        case State::upToDate:
        default:
            updates.check();
            break;
    }
}

void UpdatePanel::lookAndFeelChanged()
{
    setThemeColour (running, juce::Label::textColourId, &Palette::textMuted);
    setThemeColour (privacy, juce::Label::textColourId, &Palette::textMuted);
    refresh();
}

void UpdatePanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void UpdatePanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);
    title.setBounds (area.removeFromTop (controlHeight));
    running.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::s);

    // The switch and what it sends, along the bottom; the buttons over them.
    privacy.setBounds (area.removeFromBottom (controlHeight * 2));
    checkAtStart.setBounds (area.removeFromBottom (controlHeight));
    area.removeFromBottom (Space::m);
    auto buttons = area.removeFromBottom (controlHeight);
    action.setBounds (buttons.removeFromLeft (buttonWidth));
    buttons.removeFromLeft (Space::s);
    release.setBounds (buttons.removeFromLeft (buttonWidth));
    area.removeFromBottom (Space::s);
    progressBar.setBounds (area.removeFromBottom (controlHeight));
    status.setBounds (area);
}
} // namespace phazite::ui
