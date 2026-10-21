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

    constexpr float lightFall = 0.08f;                         // of a pad's full light, per refresh: about 0.4 s to go out
    constexpr int padSize = 72, smallestPad = 44, columns = 4; // pads: at most, and at least
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

    // Its choke group in its corner (0 for none).
    void showChoke (int group)
    {
        if (group != choke)
        {
            choke = group;
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

        if (choke > 0)
            g.drawText ("C" + juce::String (choke), inside, juce::Justification::topRight);
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
    int choke = 0;
    bool dropping = false;

    void setDropping (bool now)
    {
        dropping = now;
        repaint();
    }
};

//==============================================================================
// The selected pad's sound, drawn, with its attack's ramp and its length's cut to drag, and where it is as it plays.
class DrumSamplerEditor::Waveform : public juce::Component
{
public:
    explicit Waveform (DrumSamplerEditor& e)
        : editor (e)
    {
        setTitle ("Sound");
        setDescription ("The pad's sound. Drag the ramp at its start to change its attack, and the line where it is cut to change "
                        "its length.");
    }

    // The pad shown, and its sound's peaks, drawn again when the pad or its sound changes.
    void show (int pad)
    {
        auto* drums = editor.device.getProcessor<DrumSampler>();

        if (drums == nullptr)
            return;

        const auto& sound = drums->getPadSound (pad);

        if (pad != shownPad || &sound != shownSound)
        {
            shownPad = pad;
            shownSound = &sound;
            seconds = sound.audio.getNumSamples() / std::max (1.0, sound.sampleRate);
            attack = editor.device.getParameter (getInfo (DrumSampler::getParameterIndex (pad, Field::attack)).id);
            length = editor.device.getParameter (getInfo (DrumSampler::getParameterIndex (pad, Field::length)).id);

            // The loudest of each stretch, for a few hundred of them.
            constexpr int stretches = 400;
            peaks.assign ((size_t) stretches, 0.0f);
            const auto frames = sound.audio.getNumSamples();

            for (int channel = 0; channel < sound.audio.getNumChannels(); ++channel)
            {
                const auto* samples = sound.audio.getReadPointer (channel);

                for (int i = 0; i < frames; ++i)
                {
                    auto& peak = peaks[(size_t) std::min (stretches - 1, (int) ((juce::int64) i * stretches / std::max (1, frames)))];
                    peak = std::max (peak, std::abs (samples[i]));
                }
            }

            repaint();
        }

        if (const auto now = drums->getPlayhead (pad).get(); ! juce::approximatelyEqual (now, playhead))
        {
            playhead = now;
            repaint();
        }
    }

    float xForSeconds (double time) const
    {
        const auto area = getLocalBounds().toFloat().reduced (2.0f);
        return area.getX() + (float) (time / std::max (0.001, seconds)) * area.getWidth();
    }

    double secondsAt (float x) const
    {
        const auto area = getLocalBounds().toFloat().reduced (2.0f);
        return (double) ((x - area.getX()) / std::max (1.0f, area.getWidth())) * seconds;
    }

    float getAttackX() const { return xForSeconds (attack.getValue() * 0.001); }

    float getLengthX() const
    {
        const auto cut = length.getValue();
        return cut >= length.getRange().end ? xForSeconds (seconds) : xForSeconds (std::min (seconds, cut * 0.001));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto colour = juce::Colour (partColours[(size_t) std::max (0, shownPad) % std::size (partColours)]);
        const auto area = getLocalBounds().toFloat();
        g.setColour (p.background);
        g.fillRoundedRectangle (area, cornerRadius);

        // The sound, mirrored about the middle, faint after its cut.
        const auto inner = area.reduced (2.0f);
        const auto cutX = getLengthX();
        juce::Path shape;
        const auto stretch = inner.getWidth() / (float) std::max<size_t> (1, peaks.size());

        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const auto height = std::min (1.0f, peaks[i]) * inner.getHeight() * 0.5f;
            shape.addRectangle (inner.getX() + (float) i * stretch, inner.getCentreY() - height, std::max (1.0f, stretch), 2.0f * height);
        }

        g.setColour (colour.withAlpha (GraphTint::partCurve));
        g.fillPath (shape);
        g.saveState();
        g.reduceClipRegion (inner.withRight (cutX).toNearestInt());
        g.setColour (colour);
        g.fillPath (shape);
        g.restoreState();

        // The attack's ramp, from nothing at the start, and the cut.
        const auto attackX = getAttackX();
        g.setColour (p.text);
        g.drawLine (inner.getX(), inner.getBottom(), attackX, inner.getY(), 1.5f);
        g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ attackX, inner.getY() + 4.0f }));

        if (cutX < inner.getRight() - 1.0f)
        {
            g.setColour (p.danger);
            g.drawLine (cutX, inner.getY(), cutX, inner.getBottom(), 2.0f);
        }

        // Where it is, as it plays.
        if (playhead >= 0.0f)
        {
            g.setColour (p.text);
            const auto x = inner.getX() + playhead * inner.getWidth();
            g.drawLine (x, inner.getY(), x, inner.getBottom(), 1.5f);
        }

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (juce::String (seconds, 2) + " s", inner.reduced ((float) Space::xs), juce::Justification::bottomRight, false);
    }

    // Near the ramp's end: the attack; elsewhere, the cut.
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragged = std::abs (e.position.x - getAttackX()) < std::abs (e.position.x - getLengthX()) ? &attack : &length;
        dragged->beginGesture();
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged == nullptr)
            return;

        const auto range = dragged->getRange();
        auto value = (float) (secondsAt (e.position.x) * 1000.0);

        // The cut dragged to the end plays the whole sound.
        if (dragged == &length && secondsAt (e.position.x) >= seconds * 0.98)
            value = range.end;

        dragged->setValue (juce::jlimit (range.start, range.end, value));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged != nullptr)
            dragged->endGesture();

        dragged = nullptr;
    }

private:
    DrumSamplerEditor& editor;
    int shownPad = -1;
    const DrumSampler::Sound* shownSound = nullptr;
    double seconds = 1.0;
    std::vector<float> peaks;
    float playhead = -1.0f;
    ParameterHandle attack, length;
    ParameterHandle* dragged = nullptr;
};

//==============================================================================
namespace
{
    const auto knobCell = compactKnobSize + Space::l;
    constexpr int cells = 7; // the pad's five knobs, its choke group and the output

    int getControlsWidth()
    {
        return cells * knobCell;
    }

    int getControlsHeight()
    {
        return compactKnobSize + 2 * labelHeight;
    }
} // namespace

DrumSamplerEditor::DrumSamplerEditor (const DeviceHandle& handle)
    : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
      device (handle)
{
    for (int pad = 0; pad < numPads; ++pad)
    {
        pads[(size_t) pad] = std::make_unique<Pad> (*this, pad);
        addAndMakeVisible (*pads[(size_t) pad]);
    }

    waveform = std::make_unique<Waveform> (*this);
    addAndMakeVisible (*waveform);

    padName.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
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
    const auto height = 2 * Space::s + getGridSize (10000);
    setSize (getWidthForHeight (height), height);
}

DrumSamplerEditor::~DrumSamplerEditor() = default;

int DrumSamplerEditor::getGridSize (int height) const
{
    return juce::jlimit (columns * smallestPad + (columns - 1) * Space::xs, columns * padSize + (columns - 1) * Space::xs,
                         height - 2 * Space::s);
}

int DrumSamplerEditor::getMinimumHeight() const
{
    // The pads at their smallest, beside the name's row, a little of the waveform and the controls.
    const auto right = controlHeight + Space::s + 60 + Space::s + getControlsHeight();
    return 2 * Space::s + std::max (columns * smallestPad + (columns - 1) * Space::xs, right);
}

int DrumSamplerEditor::getWidthForHeight (int height) const
{
    return 2 * Space::s + getGridSize (height) + Space::m + getControlsWidth();
}

juce::Button& DrumSamplerEditor::getPad (int pad)
{
    return *pads[(size_t) juce::jlimit (0, numPads - 1, pad)];
}

juce::Component& DrumSamplerEditor::getWaveform() const
{
    return *waveform;
}

float DrumSamplerEditor::getAttackX() const
{
    return waveform->getAttackX();
}

float DrumSamplerEditor::getLengthX() const
{
    return waveform->getLengthX();
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
        each.showChoke (
            juce::roundToInt (device.getParameter (getInfo (DrumSampler::getParameterIndex (pad, Field::choke)).id).getValue()));
    }

    padName.setText ("Pad " + juce::String (selectedPad + 1) + ": " + device.getPadName (selectedPad), juce::dontSendNotification);
    resetButton.setEnabled (device.hasPadSound (selectedPad));
    waveform->show (selectedPad);

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
    auto area = getLocalBounds().reduced (Space::s);
    const auto gridSize = getGridSize (getHeight());
    const auto grid = area.removeFromLeft (gridSize).withSizeKeepingCentre (gridSize, gridSize);
    const auto pad = (gridSize - (columns - 1) * Space::xs) / columns;

    // The first pad at the bottom left, the last at the top right.
    for (int index = 0; index < numPads; ++index)
    {
        const auto row = numPads / columns - 1 - index / columns, column = index % columns;
        pads[(size_t) index]->setBounds (grid.getX() + column * (pad + Space::xs), grid.getY() + row * (pad + Space::xs), pad, pad);
    }

    area.removeFromLeft (Space::m);

    // The pad's name with its buttons, its sound, then its controls in a row.
    auto top = area.removeFromTop (controlHeight);
    resetButton.setBounds (top.removeFromRight (knobCell + Space::s));
    top.removeFromRight (Space::xs);
    loadButton.setBounds (top.removeFromRight (knobCell + Space::l));
    top.removeFromRight (Space::s);
    padName.setBounds (top);
    area.removeFromTop (Space::s);

    auto controls = area.removeFromBottom (getControlsHeight());
    area.removeFromBottom (Space::s);
    waveform->setBounds (area);

    // Level, pan, tune, attack, length, the choke group, then the output.
    const auto place = [this, &controls] (int slot)
    {
        auto cell = controls.removeFromLeft (knobCell);
        const auto knobArea = cell.removeFromTop (compactKnobSize);

        if (slot < numKnobs)
        {
            knobs[(size_t) slot]->setBounds (knobArea.withSizeKeepingCentre (compactKnobSize, compactKnobSize));
            names[slot]->setBounds (cell.removeFromTop (labelHeight));
            texts[slot]->setBounds (cell.removeFromTop (labelHeight));
        }
        else
        {
            chokeBox.setBounds (knobArea.withSizeKeepingCentre (knobCell - 2, controlHeight));
            names[slot]->setBounds (cell.removeFromTop (labelHeight));
        }
    };

    for (const auto slot : { 0, 1, 2, 3, 4, numKnobs, outputKnob })
        place (slot);
}
} // namespace phazite::ui
