// SPDX-License-Identifier: AGPL-3.0-or-later
// The window a freeze or a bounce shows while it renders (model/mixer/FreezeModel.h; roadmap step 2.11): what it does, how
// far it is, and Cancel, which stops it and leaves the project as it was. It owns the render.
#pragma once

#include "model/mixer/FreezeModel.h"

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class RenderPanel : public juce::Component,
                    private juce::Timer
{
public:
    explicit RenderPanel (std::unique_ptr<TrackRender>);
    ~RenderPanel() override;

    static constexpr int width = 420, height = 128;

    // Cancel stops the render (which goes) and closes the dialog window it is in; the second way out, where the render
    // has one (getGoOnName()), does what it does first; Escape is Cancel there.
    void cancel();
    void goOn();

    // While it shows a hold (TrackRender::isHold()), what runs the keys that run while a mouse button is down: the window's
    // (KeyDispatcher::keyPressedWhileHeld()).
    std::function<bool (const juce::KeyPress&)> onHeldKey;
    std::function<void (bool isKeyDown)> onHeldKeyState; // and the keys' going down and up, so that a key let go is pressed again
    juce::TextButton& getGoOnButton() noexcept { return goOnButton; }

    // Told once its Cancel has closed it (the next render waiting its turn shows then).
    std::function<void()> onCancelled;

    bool isRendering() const noexcept { return render != nullptr; }
    juce::TextButton& getCancelButton() noexcept { return cancelButton; }
    juce::ProgressBar& getProgressBar() noexcept { return progressBar; }

    // A render's wait for a plug-in on its progress bar, "Waiting for Kontakt (12 s)", in the place of its percentage, which
    // comes back once it is over; what the bar shows now is kept in shown. The export's window shows it so too.
    static void showWait (juce::ProgressBar&, juce::String& shown, const std::optional<RenderWait>&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::unique_ptr<TrackRender> render;
    double progress = 0.0; // what the progress bar shows
    juce::Label title;
    juce::ProgressBar progressBar { progress };
    juce::TextButton cancelButton { "Cancel" }, goOnButton;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    juce::String shownWait; // what the bar says of a plug-in the render waits for (showWait())

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE (RenderPanel)
};
} // namespace phazite::ui
