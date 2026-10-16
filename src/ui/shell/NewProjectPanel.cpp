// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NewProjectPanel.h"
#include "model/arrangement/Outlines.h"
#include "ui/components/ValueText.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int labelWidth = 128;
    constexpr int fieldWidth = 112;
    constexpr int boxWidth = 176;
    constexpr int buttonWidth = 96;
    constexpr int saveTextHeight = 40;    // two lines
    constexpr int outlineTextHeight = 56; // three lines
    constexpr int errorHeight = 40;

    // A menu item's ID is its place in its list, from 1. The key's and the outline's lists start with "None".
    constexpr int noneID = 1;
    constexpr int majorID = 1, minorID = 2;
    constexpr int numTimeSignatures = (int) commonTimeSignatures.size();

    juce::String getRootName (int root)
    {
        return juce::MidiMessage::getMidiNoteName (root, true, false, 0); // "C", "C#"...
    }
} // namespace

NewProjectPanel::NewProjectPanel (Create createProject)
    : create (std::move (createProject)),
      tempoField ("Tempo", minTempo, maxTempo, 1.0, NewProjectSettings().tempo)
{
    setTitle ("New Project");

    heading.setText ("New Project", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (TextSize::heading, juce::Font::bold));
    saveText.setText ("Its first save asks what to call it and where its folder goes. Everything here can change later.",
                      juce::dontSendNotification);

    for (auto* label : { &saveText, &outlineText, &error })
        label->setJustificationType (juce::Justification::topLeft);

    const std::pair<juce::Label*, const char*> labelTexts[] { { &tempoLabel, "Tempo" },
                                                              { &timeSignatureLabel, "Time signature" },
                                                              { &keyLabel, "Key" },
                                                              { &outlineLabel, "Song outline" } };

    for (const auto& [label, text] : labelTexts)
        label->setText (text, juce::dontSendNotification);

    tempoField.textFromValueFunction = [] (double bpm) { return formatTempo (bpm); };
    tempoField.valueFromTextFunction = [this] (const juce::String& typed) { return parseTempo (typed).value_or (tempoField.getValue()); };
    tempoField.updateText();
    tempoField.setTooltip ("The song's tempo, in beats per minute: drag it up or down, scroll, or click it and type");

    timeSignatureBox.setTitle ("Time signature");

    for (int i = 0; i < numTimeSignatures; ++i)
    {
        const auto timeSignature = commonTimeSignatures[(size_t) i];
        timeSignatureBox.addItem (formatTimeSignature (timeSignature.numerator, timeSignature.denominator), i + 1);

        if (timeSignature == TimeSignature {})
            timeSignatureBox.setSelectedId (i + 1, juce::dontSendNotification);
    }

    keyBox.setTitle ("Key");
    keyBox.setTooltip ("The key the piano roll shades and snaps notes to, on every track; None leaves it for later");
    keyBox.addItem ("None", noneID);

    for (int root = 0; root < 12; ++root)
        keyBox.addItem (getRootName (root), root + 2);

    keyBox.setSelectedId (noneID, juce::dontSendNotification);
    keyBox.onChange = [this] { scaleBox.setEnabled (keyBox.getSelectedId() != noneID); };

    scaleBox.setTitle ("Scale");
    scaleBox.addItem ("Major", majorID);
    scaleBox.addItem ("Minor", minorID);
    scaleBox.setSelectedId (majorID, juce::dontSendNotification);
    scaleBox.setEnabled (false);

    outlineBox.setTitle ("Song outline");
    outlineBox.setTooltip ("Lays out the song's sections with their usual lengths, each with a tip; you can change them later");
    outlineBox.addItem ("None", noneID);
    const auto& outlines = getBuiltInOutlines();

    for (size_t i = 0; i < outlines.size(); ++i)
        outlineBox.addItem (outlines[i].name, (int) i + 2);

    outlineBox.setSelectedId (noneID, juce::dontSendNotification);
    outlineBox.onChange = [this] { updateOutlineText(); };

    instrumentsButton.setToggleState (NewProjectSettings().withInstruments, juce::dontSendNotification);
    instrumentsButton.setTooltip (
        "A Drums track on the drum sampler and a Synth track on the synth, so the computer keyboard plays at once");

    cancelButton.onClick = [this] { close(); };
    createButton.onClick = [this] { createClicked(); };

    for (auto* c : std::initializer_list<juce::Component*> { &heading, &saveText, &tempoLabel, &tempoField, &timeSignatureLabel,
                                                             &timeSignatureBox, &keyLabel, &keyBox, &scaleBox, &outlineLabel, &outlineBox,
                                                             &outlineText, &instrumentsButton, &error, &cancelButton, &createButton })
        addAndMakeVisible (c);

    updateOutlineText();
    lookAndFeelChanged();

    // Once its window is on the screen, Enter makes the project (keyPressed()), until a control takes the keys.
    setWantsKeyboardFocus (true);
    juce::MessageManager::callAsync (
        [safeThis = juce::Component::SafePointer<NewProjectPanel> (this)]
        {
            if (safeThis != nullptr && safeThis->isShowing())
                safeThis->grabKeyboardFocus();
        });
}

bool NewProjectPanel::keyPressed (const juce::KeyPress& key)
{
    if (key != juce::KeyPress (juce::KeyPress::returnKey))
        return false;

    createClicked();
    return true;
}

NewProjectSettings NewProjectPanel::getSettings() const
{
    NewProjectSettings settings;
    settings.tempo = tempoField.getValue();
    settings.timeSignature = commonTimeSignatures[(size_t) juce::jlimit (0, numTimeSignatures - 1, timeSignatureBox.getSelectedId() - 1)];

    if (keyBox.getSelectedId() != noneID)
        settings.keyRoot = keyBox.getSelectedId() - 2;

    settings.scale = scaleBox.getSelectedId() == minorID ? "minor" : "major";

    if (const auto index = outlineBox.getSelectedId() - 2; juce::isPositiveAndBelow (index, (int) getBuiltInOutlines().size()))
        settings.outline = getBuiltInOutlines()[(size_t) index].id;

    settings.withInstruments = instrumentsButton.getToggleState();
    return settings;
}

void NewProjectPanel::updateOutlineText()
{
    const auto* outline = findOutline (getSettings().outline);

    if (outline == nullptr)
    {
        outlineText.setText ("The song starts without sections. Add > Song Outline lays them out later.", juce::dontSendNotification);
        return;
    }

    juce::StringArray sections;

    for (const auto& section : outline->sections)
        sections.add (section.name + " " + juce::String (section.bars));

    outlineText.setText (sections.joinIntoString (", ") + " (" + juce::String (outline->getLengthInBars()) + " bars)",
                         juce::dontSendNotification);
}

void NewProjectPanel::createClicked()
{
    if (create == nullptr)
        return;

    if (const auto result = create (getSettings()); result.failed())
    {
        error.setText (result.getErrorMessage(), juce::dontSendNotification);
        return;
    }

    close();
}

void NewProjectPanel::close()
{
    if (onClose != nullptr)
        onClose();

    if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
        window->exitModalState (0); // which deletes it
}

void NewProjectPanel::lookAndFeelChanged()
{
    const auto& p = getPalette (*this);
    saveText.setColour (juce::Label::textColourId, p.textMuted);
    outlineText.setColour (juce::Label::textColourId, p.textMuted);
    error.setColour (juce::Label::textColourId, p.danger);
    setPrimary (createButton, true);
}

void NewProjectPanel::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void NewProjectPanel::resized()
{
    auto area = getLocalBounds().reduced (Space::xl);
    heading.setBounds (area.removeFromTop (32));
    saveText.setBounds (area.removeFromTop (saveTextHeight));
    area.removeFromTop (Space::m);

    const auto row = [&area] (juce::Label& label)
    {
        auto line = area.removeFromTop (controlHeight);
        label.setBounds (line.removeFromLeft (labelWidth));
        area.removeFromTop (Space::s);
        return line;
    };

    tempoField.setBounds (row (tempoLabel).removeFromLeft (fieldWidth));
    timeSignatureBox.setBounds (row (timeSignatureLabel).removeFromLeft (fieldWidth));

    auto keys = row (keyLabel);
    keyBox.setBounds (keys.removeFromLeft (fieldWidth));
    keys.removeFromLeft (Space::s);
    scaleBox.setBounds (keys.removeFromLeft (fieldWidth));

    outlineBox.setBounds (row (outlineLabel).removeFromLeft (boxWidth));
    outlineText.setBounds (area.removeFromTop (outlineTextHeight).withTrimmedLeft (labelWidth));
    area.removeFromTop (Space::s);
    instrumentsButton.setBounds (area.removeFromTop (controlHeight));

    auto footer = area.removeFromBottom (controlHeight);
    createButton.setBounds (footer.removeFromRight (buttonWidth));
    footer.removeFromRight (Space::s);
    cancelButton.setBounds (footer.removeFromRight (buttonWidth));

    area.removeFromTop (Space::s);
    error.setBounds (area.removeFromTop (errorHeight));
}
} // namespace phazite::ui
