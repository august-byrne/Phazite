// SPDX-License-Identifier: AGPL-3.0-or-later
// The Updates window (Help > Check for Updates...; services/Updates.h): this Phazite's version, what the last check found and
// the step that comes next, in one button: Check Now, Download and Install, Cancel while it works, Install and Restart once
// the installer is downloaded and its SHA-256 checked, or Try Again; the release's page, or the list of releases; and the
// switch for a check each time Phazite opens, off until the user turns it on, with what that sends.
#pragma once

#include "services/Updates.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class UpdatePanel : public juce::Component,
                    private juce::ChangeListener
{
public:
    // Install and Restart calls install, which closes Phazite, asking to save first, and starts the installer (the main
    // window's).
    UpdatePanel (Updates&, std::function<void()> install);
    ~UpdatePanel() override;

    static constexpr int width = 540, height = 360; // the status's five lines at most

    // For tests: what the window says, its buttons, and whether the progress bar shows.
    juce::String getStatus() const { return status.getText(); }
    juce::Button& getActionButton() noexcept { return action; }
    juce::Button& getReleaseButton() noexcept { return release; }
    juce::ToggleButton& getCheckAtStartButton() noexcept { return checkAtStart; }
    bool showsProgress() const { return progressBar.isVisible(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    Updates& updates;
    const std::function<void()> install;
    juce::Label title, running, status, privacy;
    double progress = 0.0; // what the progress bar shows
    juce::ProgressBar progressBar { progress };
    juce::TextButton action, release;
    juce::ToggleButton checkAtStart { "Check for updates when Phazite opens" };

    void refresh(); // as the update check is now
    void runAction();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    JUCE_DECLARE_NON_COPYABLE (UpdatePanel)
};

// What the Updates window and the menus say of the update check: the command's name, with the version found if a newer one
// is out ("Update to Phazite 0.2.0..."), and whether the settings' icon shows a badge for it.
juce::String getUpdateCommandName (const Updates&);
bool isUpdateWaiting (const Updates&);
} // namespace phazite::ui
