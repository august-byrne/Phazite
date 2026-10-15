// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExportPanel.h"
#include "ui/components/ValueText.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

namespace phazite::ui
{
namespace
{
    constexpr int labelWidth = 112;
    constexpr int boxWidth = 176;
    constexpr int fieldWidth = 112;
    constexpr int buttonWidth = 96;
    constexpr int aboutHeight = 56; // three lines
    constexpr int hintHeight = 56;
    constexpr int statusHeight = 64;

    // A menu item's ID is its place in its list, from 1.
    constexpr int wavID = 1, flacID = 2;
    constexpr int offID = 1, normalizeID = 2;
    constexpr int numSampleRates = (int) std::size (exportSampleRates);
    constexpr int numBitDepths = (int) std::size (exportBitDepths);

    juce::String getBitDepthName (int bits)
    {
        if (bits == 16)
            return "16-bit, dithered";

        return juce::String (bits) + (bits == 32 ? "-bit float" : "-bit");
    }

    // The ID of the item whose value is closest to the one given.
    template <typename Value, std::size_t count>
    int findID (const Value (&values)[count], double value)
    {
        int best = 0;

        for (int i = 1; i < (int) count; ++i)
            if (std::abs ((double) values[i] - value) < std::abs ((double) values[best] - value))
                best = i;

        return best + 1;
    }
} // namespace

ExportPanel::ExportPanel (Settings& s, const juce::File& defaultFile, double songSeconds, StartExport start)
    : settings (s),
      startExport (std::move (start)),
      targetField ("Loudness target", minLoudnessTarget, maxLoudnessTarget, 0.5, Settings::exportLoudnessTarget.defaultValue),
      ceilingField ("True-peak ceiling", minTruePeakCeiling, maxTruePeakCeiling, 0.1, Settings::exportTruePeakCeiling.defaultValue)
{
    setTitle ("Export");

    heading.setText ("Export the Song", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    about.setText ("From its start to the end of its last bar (" + formatDuration (songSeconds)
                       + "), then the tails of notes and effects until they fall silent, " + juce::String (juce::roundToInt (tailSeconds))
                       + " seconds at most. Muted tracks stay silent.",
                   juce::dontSendNotification);
    hint.setText ("Spotify plays songs at -14 LUFS, and broadcasters use -23 (EBU R 128). Normalizing turns the whole song up or "
                  "down without limiting it, so the ceiling may keep it quieter than the target.",
                  juce::dontSendNotification);

    const std::pair<juce::Label*, const char*> labelTexts[] { { &formatLabel, "Format" },      { &sampleRateLabel, "Sample rate" },
                                                              { &bitDepthLabel, "Bit depth" }, { &loudnessLabel, "Loudness" },
                                                              { &targetLabel, "Target" },      { &ceilingLabel, "Ceiling" },
                                                              { &fileLabel, "Save as" } };

    for (const auto& [label, text] : labelTexts)
        label->setText (text, juce::dontSendNotification);

    for (auto* label : { &about, &hint, &status })
        label->setJustificationType (juce::Justification::topLeft);

    formatBox.setTitle ("Format");
    formatBox.addItem ("WAV", wavID);
    formatBox.addItem ("FLAC (lossless, smaller)", flacID);
    formatBox.onChange = [this]
    {
        updateControls();
        setFile (file);
    };

    sampleRateBox.setTitle ("Sample rate");

    for (int i = 0; i < numSampleRates; ++i)
        sampleRateBox.addItem (formatSampleRate (exportSampleRates[i]), i + 1);

    bitDepthBox.setTitle ("Bit depth");

    for (int i = 0; i < numBitDepths; ++i)
        bitDepthBox.addItem (getBitDepthName (exportBitDepths[i]), i + 1);

    loudnessBox.setTitle ("Loudness");
    loudnessBox.addItem ("Off: as it plays", offID);
    loudnessBox.addItem ("Normalize", normalizeID);
    loudnessBox.onChange = [this] { updateControls(); };

    showAsLoudness (targetField);
    targetField.setTooltip ("The song's integrated loudness (EBU R 128) after normalizing: drag it, scroll, or click it and type");
    showAsTruePeak (ceilingField);
    ceilingField.setTooltip ("Normalizing never takes the song's true peak above this");

    fileName.setTitle ("File");
    changeFileButton.setTooltip ("Chooses another file or folder to export to");
    changeFileButton.onClick = [this] { chooseFile(); };
    showButton.setButtonText (getShowInFolderText());
    showButton.setTooltip ("Shows the exported file");
    showButton.onClick = [this] { file.revealToUser(); };
    closeButton.onClick = [this]
    {
        if (isExporting())
        {
            cancel();
            return;
        }

        if (onClose != nullptr)
            onClose();

        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0); // which deletes it
    };
    exportButton.onClick = [this] { exportClicked(); };
    progressBar.setTitle ("Export progress");

    askToReplace = [this] (const juce::File& existing, std::function<void (bool)> callback)
    {
        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, "Replace " + existing.getFileName() + "?",
                                                          "It is in " + existing.getParentDirectory().getFullPathName()
                                                              + " already. Exporting replaces it.",
                                                          "Replace", "Cancel", this),
            [callback = std::move (callback)] (int result) { callback (result == 1); });
    };

    for (auto* c : std::initializer_list<juce::Component*> {
             &heading,       &about,        &formatLabel,   &formatBox,   &sampleRateLabel, &sampleRateBox,
             &bitDepthLabel, &bitDepthBox,  &loudnessLabel, &loudnessBox, &targetLabel,     &targetField,
             &ceilingLabel,  &ceilingField, &hint,          &fileLabel,   &fileName,        &changeFileButton,
             &progressBar,   &status,       &showButton,    &closeButton, &exportButton })
        addAndMakeVisible (c);

    showButton.setVisible (false); // once there is a file
    readSettings();
    setFile (defaultFile);
    updateControls();
    lookAndFeelChanged();
}

ExportPanel::~ExportPanel()
{
    stopTimer();
    running = nullptr; // cancels an export that is still going
}

ExportSettings ExportPanel::getSettings() const
{
    ExportSettings chosen;
    chosen.format = formatBox.getSelectedId() == flacID ? ExportSettings::Format::flac : ExportSettings::Format::wav;
    chosen.sampleRate = exportSampleRates[juce::jlimit (0, numSampleRates - 1, sampleRateBox.getSelectedId() - 1)];
    chosen.bitDepth = exportBitDepths[juce::jlimit (0, numBitDepths - 1, bitDepthBox.getSelectedId() - 1)];
    chosen.truePeakCeiling = ceilingField.getValue();

    if (loudnessBox.getSelectedId() == normalizeID)
        chosen.loudnessTarget = targetField.getValue();

    return chosen;
}

void ExportPanel::setFile (const juce::File& newFile)
{
    file = newFile.withFileExtension (getFileSuffix (getSettings().format));
    fileName.setText (file.getFileName() + ", in " + file.getParentDirectory().getFileName(), juce::dontSendNotification);
    fileName.setTooltip (file.getFullPathName());
}

void ExportPanel::readSettings()
{
    formatBox.setSelectedId (settings.get (Settings::exportFormat) == "flac" ? flacID : wavID, juce::dontSendNotification);
    sampleRateBox.setSelectedId (findID (exportSampleRates, settings.get (Settings::exportSampleRate)), juce::dontSendNotification);
    bitDepthBox.setSelectedId (findID (exportBitDepths, settings.get (Settings::exportBitDepth)), juce::dontSendNotification);
    loudnessBox.setSelectedId (settings.get (Settings::exportNormalize) ? normalizeID : offID, juce::dontSendNotification);
    targetField.setValue (settings.get (Settings::exportLoudnessTarget), juce::dontSendNotification);
    ceilingField.setValue (settings.get (Settings::exportTruePeakCeiling), juce::dontSendNotification);
}

void ExportPanel::writeSettings()
{
    const auto chosen = getSettings();
    settings.set (Settings::exportFormat, chosen.format == ExportSettings::Format::flac ? "flac" : "wav");
    settings.set (Settings::exportSampleRate, chosen.sampleRate);
    settings.set (Settings::exportBitDepth, chosen.bitDepth);
    settings.set (Settings::exportNormalize, chosen.loudnessTarget.has_value());
    settings.set (Settings::exportLoudnessTarget, targetField.getValue());
    settings.set (Settings::exportTruePeakCeiling, ceilingField.getValue());
}

void ExportPanel::updateControls()
{
    const auto busy = isExporting();
    const auto flac = formatBox.getSelectedId() == flacID;
    constexpr int floatID = numBitDepths; // 32-bit float, the last: WAV only

    bitDepthBox.setItemEnabled (floatID, ! flac);

    if (flac && bitDepthBox.getSelectedId() == floatID)
        bitDepthBox.setSelectedId (floatID - 1, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &formatBox, &sampleRateBox, &bitDepthBox, &loudnessBox, &changeFileButton })
        c->setEnabled (! busy);

    const auto normalize = ! busy && loudnessBox.getSelectedId() == normalizeID;

    for (auto* c : std::initializer_list<juce::Component*> { &targetLabel, &targetField, &ceilingLabel, &ceilingField })
        c->setEnabled (normalize);

    exportButton.setVisible (! busy);
    closeButton.setButtonText (busy ? "Cancel" : (exported ? "Close" : "Cancel"));
    closeButton.setTooltip (busy ? "Stops the export" : juce::String());
    progressBar.setVisible (busy);
    resized();
}

void ExportPanel::chooseFile()
{
    const auto suffix = getFileSuffix (getSettings().format);
    chooser = std::make_unique<juce::FileChooser> ("Export the song as", file, "*" + suffix);

    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safeThis = juce::Component::SafePointer<ExportPanel> (this)] (const juce::FileChooser& c)
                          {
                              if (safeThis == nullptr || c.getResult() == juce::File())
                                  return;

                              safeThis->setFile (c.getResult());

                              // The file chooser has asked already.
                              if (safeThis->file.existsAsFile())
                                  safeThis->confirmedFile = safeThis->file;
                          });
}

void ExportPanel::exportClicked()
{
    if (! file.existsAsFile() || file == confirmedFile)
    {
        start();
        return;
    }

    askToReplace (file,
                  [safeThis = juce::Component::SafePointer<ExportPanel> (this), asked = file] (bool replace)
                  {
                      if (safeThis != nullptr && replace && safeThis->file == asked)
                      {
                          safeThis->confirmedFile = asked;
                          safeThis->start();
                      }
                  });
}

void ExportPanel::start()
{
    if (isExporting() || startExport == nullptr)
        return;

    const auto chosen = getSettings();
    writeSettings();
    showButton.setVisible (false);
    status.setText ("Exporting " + file.getFileName() + "...", juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    progress = 0.0;

    running = startExport (chosen, file,
                           [safeThis = juce::Component::SafePointer<ExportPanel> (this), chosen] (const ExportReport& report)
                           {
                               if (safeThis != nullptr)
                                   safeThis->finished (chosen, report);
                           });

    if (running != nullptr)
        startTimerHz (10);

    updateControls();
}

void ExportPanel::cancel()
{
    stopTimer();
    running = nullptr;
    status.setText ("Export cancelled.", juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    updateControls();
}

void ExportPanel::finished (const ExportSettings& chosen, const ExportReport& report)
{
    stopTimer();
    running = nullptr;
    const auto& p = getPalette (*this);

    if (report.result.failed())
    {
        status.setText ("Phazite couldn't export the song: " + report.result.getErrorMessage(), juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, p.danger);
    }
    else
    {
        exported = true;
        confirmedFile = report.file; // exporting again replaces it without asking
        auto text = "Exported " + report.file.getFileName() + ": " + formatDuration (report.seconds);

        if (report.integratedLufs > -100.0)
            text << ", " << formatLoudness (report.integratedLufs) << ", true peak " << formatTruePeak (report.truePeakDb) << ".";
        else
            text << ", too quiet to measure its loudness.";

        if (const auto shortfall = getLoudnessShortfall (chosen, report); shortfall > 0.0)
            text << " The ceiling kept it " << juce::String (shortfall, 1)
                 << " LU under the target: a limiter at the end of the master's devices can make it louder.";

        status.setText (text, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, p.text);
    }

    showButton.setVisible (report.result.wasOk());
    updateControls();
}

void ExportPanel::timerCallback()
{
    if (running != nullptr)
        progress = running->getProgress();
}

void ExportPanel::lookAndFeelChanged()
{
    const auto& p = getPalette (*this);

    for (auto* label : { &about, &hint })
        label->setColour (juce::Label::textColourId, p.textMuted);

    setPrimary (exportButton, true);
}

void ExportPanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void ExportPanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);
    heading.setBounds (area.removeFromTop (32));
    about.setBounds (area.removeFromTop (aboutHeight));
    area.removeFromTop (Space::l);

    const auto row = [&area] (juce::Label& label, juce::Component& control, int width)
    {
        auto line = area.removeFromTop (controlHeight);
        label.setBounds (line.removeFromLeft (labelWidth));
        control.setBounds (line.removeFromLeft (width));
        area.removeFromTop (Space::s);
        return line;
    };

    row (formatLabel, formatBox, boxWidth);
    row (sampleRateLabel, sampleRateBox, boxWidth);
    row (bitDepthLabel, bitDepthBox, boxWidth);
    row (loudnessLabel, loudnessBox, boxWidth);

    auto levels = row (targetLabel, targetField, fieldWidth);
    levels.removeFromLeft (Space::l);
    ceilingLabel.setBounds (levels.removeFromLeft (labelWidth / 2 + Space::l));
    ceilingField.setBounds (levels.removeFromLeft (fieldWidth));

    hint.setBounds (area.removeFromTop (hintHeight));
    area.removeFromTop (Space::s);

    auto fileRow = row (fileLabel, fileName, 0);
    changeFileButton.setBounds (fileRow.removeFromRight (buttonWidth));
    fileName.setBounds (fileRow.withTrimmedRight (Space::s));

    auto footer = area.removeFromBottom (controlHeight);
    area.removeFromBottom (Space::l);
    exportButton.setBounds (footer.removeFromRight (buttonWidth));
    footer.removeFromRight (Space::s);
    closeButton.setBounds (footer.removeFromRight (buttonWidth));
    showButton.setBounds (footer.removeFromLeft (juce::jmax (buttonWidth, showButton.getBestWidthForHeight (controlHeight))));

    area.removeFromTop (Space::s);

    if (progressBar.isVisible())
    {
        progressBar.setBounds (area.removeFromTop (controlHeight));
        area.removeFromTop (Space::s);
    }

    status.setBounds (area.removeFromTop (statusHeight));
}
} // namespace phazite::ui
