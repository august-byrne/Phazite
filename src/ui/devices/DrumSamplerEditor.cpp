// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumSamplerEditor.h"
#include "devices/drums/DrumSampler.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

#include <iterator>

namespace phazite::ui
{
namespace
{
    using devices::DrumSampler;
    using Field = DrumSampler::Field;

    constexpr int refreshRateHz = 30;
    constexpr float lightFall = 0.08f; // of a pad's full light, per refresh: about 0.4 s to go out
    constexpr int padSize = 72, columns = 4;
    constexpr Field knobFields[] = { Field::level, Field::pan, Field::tune, Field::attack, Field::length }; // then the output
    constexpr int outputKnob = 5;

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return DrumSampler::info.parameters[(size_t) index];
    }

    // "Pad 3 Level" under a pad's controls is "Level".
    juce::String getFieldName (const devices::ParameterInfo& parameter)
    {
        return toString (parameter.name).fromFirstOccurrenceOf (" ", false, false).fromFirstOccurrenceOf (" ", false, false);
    }

    std::unique_ptr<juce::Label> makeLabel (const juce::String& text)
    {
        auto label = std::make_unique<juce::Label> (juce::String(), text);
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        return label;
    }
} // namespace

//==============================================================================
class DrumSamplerEditor::Pad : public juce::Button,
                               public juce::DragAndDropTarget,
                               public juce::FileDragAndDropTarget
{
public:
    Pad (DrumSamplerEditor& e, int index)
        : juce::Button ("Pad " + juce::String (index + 1)),
          editor (e),
          pad (index)
    {
        setTitle ("Pad " + juce::String (pad + 1));
        setTooltip ("Plays the pad and shows its controls; drop a sound on it to play that instead");
        setWantsKeyboardFocus (true);
        onClick = [this] { editor.play (pad); };
    }

    void showSound (const juce::String& name)
    {
        if (name != shownName)
        {
            shownName = name;
            setDescription (name);
            repaint();
        }
    }

    // Lights up when hit, and goes out slowly.
    void light (float hit)
    {
        const auto shown = std::max (hit, brightness - lightFall);

        if (std::abs (shown - brightness) > 0.001f)
        {
            brightness = std::max (0.0f, shown);
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& palette = getPalette (*this);
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto colour = juce::Colour (partColours[(size_t) pad % std::size (partColours)]);
        auto fill =
            palette.surfaceRaised.interpolatedWith (colour, GraphTint::partCurve + (GraphTint::padLit - GraphTint::partCurve) * brightness);

        if (highlighted || down)
            fill = fill.brighter (0.1f);

        g.setColour (fill);
        g.fillRoundedRectangle (area, cornerRadius);

        const auto selected = pad == editor.selectedPad;
        g.setColour (selected || dropping || hasKeyboardFocus (false) ? palette.accent : palette.border);
        g.drawRoundedRectangle (area, cornerRadius, selected || dropping ? 2.0f : 1.0f);

        // Its number in the corner, and what it plays.
        const auto inside = area.reduced ((float) Space::xs);
        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (palette.textMuted);
        g.drawText (juce::String (pad + 1), inside, juce::Justification::topLeft);
        g.setColour (palette.text);
        g.drawFittedText (shownName, inside.toNearestInt(), juce::Justification::centred, 2);
    }

    // From the browser: a sound file.
    bool isInterestedInDragSource (const SourceDetails& details) override { return getDraggedFile (details.description).has_value(); }
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }

    void itemDropped (const SourceDetails& details) override
    {
        setDropping (false);

        if (const auto file = getDraggedFile (details.description); file.has_value() && editor.dropFile (pad, *file))
            markPlaced (details.description);
    }

    // From Explorer.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override { setDropping (true); }
    void fileDragExit (const juce::StringArray&) override { setDropping (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDropping (false);
        editor.dropFile (pad, juce::File (files[0]));
    }

private:
    DrumSamplerEditor& editor;
    const int pad;
    juce::String shownName;
    float brightness = 0.0f;
    bool dropping = false;

    void setDropping (bool now)
    {
        dropping = now;
        repaint();
    }
};

//==============================================================================
DrumSamplerEditor::DrumSamplerEditor (const DeviceHandle& handle)
    : device (handle)
{
    for (int pad = 0; pad < numPads; ++pad)
    {
        pads[(size_t) pad] = std::make_unique<Pad> (*this, pad);
        addAndMakeVisible (*pads[(size_t) pad]);
    }

    padName.setFont (juce::FontOptions (TextSize::title));
    addAndMakeVisible (padName);

    loadButton.setTooltip ("Picks a sound file for the pad; the project keeps a copy of it");
    loadButton.onClick = [this] { chooseFile(); };
    resetButton.setTooltip ("Puts the kit's own sound back on the pad");
    resetButton.onClick = [this]
    {
        device.clearPadSound (selectedPad);
        refresh();
    };
    addAndMakeVisible (loadButton);
    addAndMakeVisible (resetButton);

    for (int i = 0; i < numKnobs; ++i)
    {
        const auto& parameter =
            i == outputKnob ? getInfo (DrumSampler::outputParameter) : getInfo (DrumSampler::getParameterIndex (0, knobFields[i]));
        knobs[(size_t) i] = std::make_unique<Knob> (toString (parameter.name), parameter.defaultValue);
        attachments[(size_t) i] = std::make_unique<ParameterAttachment> (*knobs[(size_t) i]);
        addAndMakeVisible (*knobs[(size_t) i]);
        addAndMakeVisible (*names.add (makeLabel (i == outputKnob ? juce::String ("Output") : getFieldName (parameter)).release()));
        addAndMakeVisible (*texts.add (makeLabel ({}).release()));
        texts.getLast()->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    }

    showAsParameter (*knobs[outputKnob], getInfo (DrumSampler::outputParameter));
    attachments[outputKnob]->setParameter (device.getParameter (getInfo (DrumSampler::outputParameter).id));
    addAndMakeVisible (chokeBox);
    addAndMakeVisible (*names.add (makeLabel ("Choke").release()));

    // The pads' first readings would light them for what played before the editor opened.
    if (auto* drums = device.getProcessor<DrumSampler>())
        for (int pad = 0; pad < numPads; ++pad)
            drums->getPadMeter (pad).take();

    selectPad (0);
    const auto grid = columns * padSize + (columns - 1) * Space::s;
    setSize (2 * Space::l + grid + Space::xl + 4 * (knobSize + Space::l), 2 * Space::l + grid);
    startTimerHz (refreshRateHz);
}

DrumSamplerEditor::~DrumSamplerEditor()
{
    stopTimer();
}

juce::Button& DrumSamplerEditor::getPad (int pad)
{
    return *pads[(size_t) juce::jlimit (0, numPads - 1, pad)];
}

void DrumSamplerEditor::selectPad (int pad)
{
    selectedPad = juce::jlimit (0, numPads - 1, pad);

    for (int i = 0; i < outputKnob; ++i)
    {
        const auto& parameter = getInfo (DrumSampler::getParameterIndex (selectedPad, knobFields[i]));
        knobs[(size_t) i]->setTitle (toString (parameter.name));
        showAsParameter (*knobs[(size_t) i], parameter);
        attachments[(size_t) i]->setParameter (device.getParameter (parameter.id));
    }

    const auto& choke = getInfo (DrumSampler::getParameterIndex (selectedPad, Field::choke));
    chokeBox.setTitle (toString (choke.name));
    chokeAttachment.setParameter (device.getParameter (choke.id));

    for (auto& each : pads)
        each->repaint();

    refresh();
}

void DrumSamplerEditor::play (int pad)
{
    selectPad (pad);

    if (auto* drums = device.getProcessor<DrumSampler>())
        drums->audition (pad, 1.0f);
}

bool DrumSamplerEditor::dropFile (int pad, const juce::File& file)
{
    selectPad (pad);
    const auto put = device.setPadSound (pad, file);
    refresh();
    return put;
}

void DrumSamplerEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a sound for pad " + juce::String (selectedPad + 1), juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<DrumSamplerEditor> (this), pad = selectedPad] (const juce::FileChooser& chosen)
        {
            if (safe != nullptr && chosen.getResult() != juce::File())
                safe->dropFile (pad, chosen.getResult());
        });
}

void DrumSamplerEditor::refresh()
{
    auto* drums = device.getProcessor<DrumSampler>();

    for (int pad = 0; pad < numPads; ++pad)
    {
        auto& each = *pads[(size_t) pad];
        each.showSound (device.getPadName (pad));
        each.light (drums != nullptr ? drums->getPadMeter (pad).take() : 0.0f);
    }

    padName.setText ("Pad " + juce::String (selectedPad + 1) + ": " + device.getPadName (selectedPad), juce::dontSendNotification);
    resetButton.setEnabled (device.hasPadSound (selectedPad));

    for (int i = 0; i < numKnobs; ++i)
    {
        attachments[(size_t) i]->update();
        texts[i]->setText (knobs[(size_t) i]->getTextFromValue (knobs[(size_t) i]->getValue()), juce::dontSendNotification);
    }

    chokeAttachment.update();
}

void DrumSamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void DrumSamplerEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::l);
    const auto grid = area.removeFromLeft (columns * padSize + (columns - 1) * Space::s);

    // The first pad at the bottom left, the last at the top right.
    for (int pad = 0; pad < numPads; ++pad)
    {
        const auto row = numPads / columns - 1 - pad / columns, column = pad % columns;
        pads[(size_t) pad]->setBounds (grid.getX() + column * (padSize + Space::s), grid.getY() + row * (padSize + Space::s), padSize,
                                       padSize);
    }

    area.removeFromLeft (Space::xl);
    padName.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::s);
    auto buttons = area.removeFromTop (controlHeight);
    loadButton.setBounds (buttons.removeFromLeft (2 * knobSize));
    buttons.removeFromLeft (Space::s);
    resetButton.setBounds (buttons.removeFromLeft (knobSize + Space::l));
    area.removeFromTop (Space::m);

    // Level, pan, tune and the output; then attack, length and the choke group.
    const auto cellHeight = knobSize + 2 * labelHeight;
    const auto place = [this, &area, cellHeight] (int slot, int line, int column)
    {
        auto cell = juce::Rectangle<int> (area.getX() + column * (knobSize + Space::l), area.getY() + line * cellHeight,
                                          knobSize + Space::l, cellHeight);
        const auto top = cell.removeFromTop (knobSize);

        if (slot < numKnobs)
        {
            knobs[(size_t) slot]->setBounds (top.withSizeKeepingCentre (knobSize, knobSize));
            texts[slot]->setBounds (cell.withTrimmedTop (labelHeight).removeFromTop (labelHeight));
        }
        else
        {
            chokeBox.setBounds (top.withSizeKeepingCentre (knobSize + Space::s, controlHeight));
        }

        names[slot]->setBounds (cell.removeFromTop (labelHeight));
    };

    place (0, 0, 0);
    place (1, 0, 1);
    place (2, 0, 2);
    place (outputKnob, 0, 3);
    place (3, 1, 0);
    place (4, 1, 1);
    place (numKnobs, 1, 2); // the choke group
}
} // namespace phazite::ui
