// SPDX-License-Identifier: AGPL-3.0-or-later
// The new-project window (docs/design.md, section 13; roadmap step 1.9): the project's name, its tempo, time signature
// and key, a song outline that lays out its sections, and whether it starts with a drum track and a synth track.
#pragma once

#include "model/project/NewProject.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class NewProjectPanel : public juce::Component
{
public:
    // Makes the project (the app's), or says why it can't, such as a folder of that name being there already.
    using Create = std::function<juce::Result (const juce::String& name, const NewProjectSettings&)>;

    // suggestedName: the name it starts with. folder: where the project's own folder goes, which the window names.
    NewProjectPanel (const juce::String& suggestedName, const juce::File& folder, Create);

    juce::String getProjectName() const { return nameEditor.getText().trim(); }
    NewProjectSettings getSettings() const; // as the controls show them

    // Cancel, or a project made, which also closes the dialog window it is in.
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    // For tests.
    juce::TextEditor& getNameEditor() noexcept { return nameEditor; }
    NumberField& getTempoField() noexcept { return tempoField; }
    juce::ComboBox& getTimeSignatureBox() noexcept { return timeSignatureBox; }
    juce::ComboBox& getKeyBox() noexcept { return keyBox; }
    juce::ComboBox& getScaleBox() noexcept { return scaleBox; }
    juce::ComboBox& getOutlineBox() noexcept { return outlineBox; }
    juce::ToggleButton& getInstrumentsButton() noexcept { return instrumentsButton; }
    juce::TextButton& getCreateButton() noexcept { return createButton; }
    juce::TextButton& getCancelButton() noexcept { return cancelButton; }
    juce::String getOutlineText() const { return outlineText.getText(); }
    juce::String getError() const { return error.getText(); }

private:
    const Create create;
    juce::Label heading, folderLabel, nameLabel, tempoLabel, timeSignatureLabel, keyLabel, outlineLabel, outlineText, error;
    juce::TextEditor nameEditor;
    NumberField tempoField;
    juce::ComboBox timeSignatureBox, keyBox, scaleBox, outlineBox;
    juce::ToggleButton instrumentsButton { "Start with a drum track and a synth track, ready to play" };
    juce::TextButton cancelButton { "Cancel" }, createButton { "Create" };

    void updateOutlineText();
    void createClicked();
    void close();
};
} // namespace phazite::ui
