// SPDX-License-Identifier: AGPL-3.0-or-later
// The Export window (docs/design.md, section 18; roadmap step 1.8): what it exports, the song or its stems (a file for
// each channel that reaches the master, step 2.11), how they are written (WAV or FLAC, the sample rate and bit depth, and
// for the song a loudness to normalize it to), where they go, how far the export is, and what it wrote. The choices are
// kept in the settings for the next export.
#pragma once

#include "model/project/Export.h"
#include "services/Settings.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class ExportPanel : public juce::Component,
                    private juce::Timer
{
public:
    // Starts an export of the open project's song or stems with the settings to the file or folder; the export calls back
    // on the message thread when it is done, and deleting it first cancels it (model/project/Export.h).
    using StartExport = std::function<std::unique_ptr<SongExport> (const ExportSettings&, const juce::File&,
                                                                   std::function<void (const ExportReport&)> onFinished)>;

    // defaultFile: where the song goes unless another file is chosen, with the chosen format's suffix; its stems go into
    // the folder next to it (getStemsFolder()). songSeconds: how long the song is, up to its end. stemNames: the channels
    // its stems are, in the mixer's order.
    ExportPanel (Settings&, const juce::File& defaultFile, double songSeconds, const juce::StringArray& stemNames, StartExport);
    ~ExportPanel() override;

    // Asks whether to replace a file or folder that is there already, and calls back with the answer. An alert window,
    // unless replaced (tests).
    std::function<void (const juce::File&, std::function<void (bool replace)>)> askToReplace;

    // Close (or Cancel before an export), which also closes the dialog window it is in.
    std::function<void()> onClose;

    ExportSettings getSettings() const; // as the controls show them
    bool isExportingStems() const;      // what the What box shows
    juce::File getFile() const noexcept { return file; }
    juce::File getFolder() const noexcept { return folder; } // where the stems go
    juce::File getDestination() const { return isExportingStems() ? folder : file; }
    void setFile (const juce::File&);   // given the chosen format's suffix
    void setFolder (const juce::File&); // for the stems
    bool isExporting() const noexcept { return running != nullptr; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    // For tests.
    juce::ComboBox& getWhatBox() noexcept { return whatBox; }
    juce::ComboBox& getFormatBox() noexcept { return formatBox; }
    juce::ComboBox& getSampleRateBox() noexcept { return sampleRateBox; }
    juce::ComboBox& getBitDepthBox() noexcept { return bitDepthBox; }
    juce::ComboBox& getLoudnessBox() noexcept { return loudnessBox; }
    NumberField& getTargetField() noexcept { return targetField; }
    NumberField& getCeilingField() noexcept { return ceilingField; }
    juce::TextButton& getExportButton() noexcept { return exportButton; }
    juce::TextButton& getCloseButton() noexcept { return closeButton; }
    juce::TextButton& getShowButton() noexcept { return showButton; }
    juce::String getStatus() const { return status.getText(); }
    juce::String getAbout() const { return about.getText(); }

private:
    Settings& settings;
    const StartExport startExport;
    const double songSeconds;
    const juce::StringArray stemNames;
    juce::File file, folder;
    juce::File confirmedFile; // one to replace without asking: chosen in the file chooser, which asked, or exported here
    juce::File exportedTo;    // what the last export wrote, which Show shows
    bool exported = false;    // since the window opened
    std::unique_ptr<SongExport> running;
    std::unique_ptr<juce::FileChooser> chooser;
    double progress = 0.0; // what the progress bar shows

    juce::Label heading, about, whatLabel, formatLabel, sampleRateLabel, bitDepthLabel, loudnessLabel, targetLabel, ceilingLabel, hint,
        fileLabel, fileName, status;
    juce::ComboBox whatBox, formatBox, sampleRateBox, bitDepthBox, loudnessBox;
    NumberField targetField, ceilingField;
    juce::TextButton changeFileButton { "Change..." }, showButton, closeButton { "Cancel" }, exportButton { "Export" };
    juce::ProgressBar progressBar { progress };

    void readSettings();
    void writeSettings();
    void updateTexts();    // the heading, what it says about the song or the stems, and where they go
    void updateControls(); // which can be used, and the bit depths the format offers
    void chooseFile();
    void exportClicked();
    void start();
    void cancel();
    void finished (const ExportSettings&, const ExportReport&);
    void timerCallback() override; // the progress
};
} // namespace phazite::ui
