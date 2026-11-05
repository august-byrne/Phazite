// SPDX-License-Identifier: AGPL-3.0-or-later
// The window of Clean Up Unused Audio... (model/project/UnusedAudio.h): the audio files in the project's folder that no song
// of it plays, each ticked, with its size, and what the ticked ones take. Delete moves them to the recycle bin and says how
// many went; Cancel (then Close) closes the window.
#pragma once

#include "model/project/ProjectContext.h"
#include "model/project/UnusedAudio.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class CleanUpPanel : public juce::Component,
                     private juce::ListBoxModel
{
public:
    // Deleting into the recycle bin, or (the tests') outright.
    explicit CleanUpPanel (ProjectContext&, Removal = Removal::toRecycleBin);

    static constexpr int width = 520, height = 440;

    // For tests: the files listed, a row at a time as screen readers say it, and their ticks; what the panel says of them.
    int getNumFiles() const noexcept { return (int) files.size(); }
    juce::String describeFile (int row) const;
    bool isTicked (int row) const;
    void setTicked (int row, bool);
    juce::String getSummary() const { return summary.getText(); }
    juce::ListBox& getList() noexcept { return list; }
    juce::Button& getDeleteButton() noexcept { return deleteButton; }
    juce::Button& getCancelButton() noexcept { return cancelButton; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ProjectContext& context;
    const Removal removal;
    std::vector<UnusedAudioFile> files;
    std::vector<bool> ticked;
    juce::Label title, summary;
    juce::ListBox list;
    juce::TextButton deleteButton { "Delete" }, cancelButton { "Cancel" };

    void findFiles();
    void deleteTicked();
    void refreshSummary (const juce::String& done = {});
    void close();

    int getNumRows() override { return getNumFiles(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    juce::String getNameForRow (int row) override { return describeFile (row); }

    JUCE_DECLARE_NON_COPYABLE (CleanUpPanel)
};
} // namespace phazite::ui
