// SPDX-License-Identifier: AGPL-3.0-or-later
// The window a freeze or a bounce shows while it renders (model/mixer/FreezeModel.h; roadmap step 2.11): what it does, how
// far it is, and Cancel, which stops it and leaves the project as it was. It owns the render.
#pragma once

#include "model/mixer/FreezeModel.h"

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

    // Cancel stops the render (which goes) and closes the dialog window it is in.
    void cancel();

    bool isRendering() const noexcept { return render != nullptr; }
    juce::TextButton& getCancelButton() noexcept { return cancelButton; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::unique_ptr<TrackRender> render;
    double progress = 0.0; // what the progress bar shows
    juce::Label title;
    juce::ProgressBar progressBar { progress };
    juce::TextButton cancelButton { "Cancel" };

    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE (RenderPanel)
};
} // namespace phazite::ui
