// SPDX-License-Identifier: AGPL-3.0-or-later
// The Chord Progressions window (docs/design.md, section 14; roadmap step 2.7): the progressions that ship with Phazite,
// by mood and genre, each with its name, its chords in the song's key, its numerals and its feel. Play hears the selected
// one in the song's key and tempo, over and over; Put on the Chord Lane lays it over the selected sections, the selected
// time range or the whole song, as one undo step, and the parts made from the chords follow.
#pragma once

#include "model/chords/Progressions.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite
{
class Previewer;
class ProjectContext;
} // namespace phazite

namespace phazite::ui
{
class ProgressionsPanel : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::Timer
{
public:
    ProgressionsPanel (ProjectContext&, Previewer&);
    ~ProgressionsPanel() override;

    // Where a progression goes: what is selected, or the whole song.
    enum class Target
    {
        selectedSections,
        selectedRange,
        wholeSong
    };

    // The progressions shown, with the mood and genre chosen (empty: any).
    void setFilter (const juce::String& mood, const juce::String& genre);
    const std::vector<const Progression*>& getShown() const noexcept { return shown; }

    void select (const Progression*);
    const Progression* getSelected() const;

    // Plays the selected progression in the song's key where it would go, or stops it.
    void play();
    void stop();
    bool isPlaying() const;

    // The targets there are now, the first being where it goes unless another is chosen; and putting it there.
    std::vector<Target> getTargets() const;
    void setTarget (Target);
    void apply();

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override; // the targets, as the selection is when the window shows

    // For tests.
    juce::ListBox& getList() noexcept { return list; }
    juce::ComboBox& getMoodBox() noexcept { return moodBox; }
    juce::ComboBox& getGenreBox() noexcept { return genreBox; }
    juce::ComboBox& getTargetBox() noexcept { return targetBox; }
    juce::TextButton& getPlayButton() noexcept { return playButton; }
    juce::TextButton& getApplyButton() noexcept { return applyButton; }

private:
    ProjectContext& context;
    Previewer& previewer;
    std::vector<const Progression*> shown;
    std::vector<Target> targets; // in targetBox's order
    juce::File previewFile;

    juce::Label heading, about, moodLabel, genreLabel, targetLabel;
    juce::ComboBox moodBox, genreBox, targetBox;
    juce::ListBox list { "Progressions", this };
    juce::TextButton playButton { "Play" }, applyButton { "Put on the Chord Lane" };

    std::optional<BeatRange> getTargetRange (Target) const;
    BeatRange getRangeFor (const Progression&) const; // where the selected target is, or once from the start
    Key getKey() const;                               // the lane's key where the progression would go
    void updateTargets();
    void updateButtons();
    void filterChanged();

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    juce::String getNameForRow (int row) override;

    void timerCallback() override; // the play button, as the previewer plays

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgressionsPanel)
};
} // namespace phazite::ui
