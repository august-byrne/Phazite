// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WavetableEditor.h"
#include "DeviceTint.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::WavetableSynth;

    constexpr int pointsPerWave = 72; // where each frame's wave is drawn through

    std::string_view getParameterID (int index)
    {
        return WavetableSynth::info.parameters[(size_t) index].id;
    }

    std::string_view getParameterID (int osc, WavetableSynth::OscillatorField field)
    {
        return getParameterID (WavetableSynth::getParameter (osc, field));
    }

    juce::String getOscillatorName (int osc)
    {
        return "Osc " + juce::String (osc + 1);
    }
} // namespace

//==============================================================================
class WavetableEditor::TableView : public juce::Component,
                                   public juce::SettableTooltipClient,
                                   public juce::DragAndDropTarget,
                                   public juce::FileDragAndDropTarget
{
public:
    TableView (WavetableEditor& e, int index, ParameterHandle positionHandle)
        : editor (e),
          osc (index),
          position (std::move (positionHandle))
    {
        setTitle (getOscillatorName (osc) + " Wavetable");
        setTooltip ("Drag across it to move through the table; drop a sound on it to make that the table");
    }

    void setTable (std::shared_ptr<const devices::Wavetable> newTable, const juce::String& name)
    {
        if (newTable == table)
            return;

        table = std::move (newTable);
        setDescription (name);
        rebuild();
        repaint();
    }

    void setPlayed (float newPosition, bool isOn)
    {
        if (std::abs (newPosition - played) > 0.0005f || isOn != on)
        {
            played = newPosition;
            on = isOn;
            repaint();
        }
    }

    float getPlayedPosition() const noexcept { return played; }
    int getNumFramesDrawn() const noexcept { return (int) frames.size(); }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        const auto area = getLocalBounds().toFloat();
        g.setColour (palette.surfaceRaised);
        g.fillRoundedRectangle (area, cornerRadius);

        if (dropping)
        {
            g.setColour (palette.accent.withAlpha (WavetableTint::dropWash));
            g.fillRoundedRectangle (area, cornerRadius);
        }

        g.setColour (dropping ? palette.accent : palette.border);
        g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);

        // The frames, the back ones first, so that those in front cover them.
        const auto alpha = on ? 1.0f : GraphTint::partOff;
        g.setColour (palette.textMuted.withAlpha (WavetableTint::frames * alpha));

        for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame)
            g.strokePath (*frame, juce::PathStrokeType (WavetableTint::lineWidth));

        // The one it plays.
        if (table != nullptr)
        {
            const auto place = std::clamp (played, 0.0f, 1.0f) * (float) (table->getNumFrames() - 1);
            const auto first = std::min ((int) place, table->getNumFrames() - 1);
            const auto second = std::min (first + 1, table->getNumFrames() - 1);
            g.setColour (getDeviceTint (*this).withAlpha (alpha));
            g.strokePath (makeWave (table->getFrameForDrawing (first), table->getFrameForDrawing (second), place - (float) first,
                                    getFrameBox (std::clamp (played, 0.0f, 1.0f))),
                          juce::PathStrokeType (WavetableTint::playedWidth));
        }
    }

    void resized() override { rebuild(); }

    // A drag across it moves the oscillator's position: the width of the drawing, right or up, is the whole table.
    void mouseDown (const juce::MouseEvent&) override
    {
        position.beginGesture();
        dragStart = position.getValue();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto moved = (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / (float) std::max (1, getWidth());
        position.setValue (std::clamp (dragStart + moved, 0.0f, 1.0f));
    }

    void mouseUp (const juce::MouseEvent&) override { position.endGesture(); }

    // From the browser: a sound file.
    bool isInterestedInDragSource (const SourceDetails& details) override { return getDraggedFile (details.description).has_value(); }
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }

    void itemDropped (const SourceDetails& details) override
    {
        setDropping (false);

        if (const auto file = getDraggedFile (details.description); file.has_value() && editor.dropFile (osc, *file))
            markPlaced (details.description);
    }

    // From Explorer.
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
    void fileDragEnter (const juce::StringArray&, int, int) override { setDropping (true); }
    void fileDragExit (const juce::StringArray&) override { setDropping (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDropping (false);
        editor.dropFile (osc, juce::File (files[0]));
    }

private:
    WavetableEditor& editor;
    const int osc;
    ParameterHandle position;
    std::shared_ptr<const devices::Wavetable> table;
    std::vector<juce::Path> frames; // those drawn, from the front
    float played = 0.0f, dragStart = 0.0f;
    bool on = true, dropping = false;

    void setDropping (bool now)
    {
        dropping = now;
        repaint();
    }

    // Where a frame is drawn: the first at the front, at the bottom left, and the last at the back, at the top right.
    juce::Rectangle<float> getFrameBox (float depth) const
    {
        const auto area = getLocalBounds().toFloat().reduced ((float) Space::s);
        const auto width = area.getWidth() * (1.0f - WavetableTint::shift);
        const auto height = area.getHeight() * (1.0f - WavetableTint::rise);
        return { area.getX() + depth * WavetableTint::shift * area.getWidth(),
                 area.getY() + (1.0f - depth) * WavetableTint::rise * area.getHeight(), width, height };
    }

    // A frame's wave, or a mix of two, across a box.
    static juce::Path makeWave (std::span<const float> from, std::span<const float> to, float morph, juce::Rectangle<float> box)
    {
        juce::Path wave;

        for (int i = 0; i <= pointsPerWave; ++i)
        {
            const auto along = (float) i / (float) pointsPerWave;
            const auto read = [along] (std::span<const float> samples)
            { return samples.empty() ? 0.0f : samples[std::min (samples.size() - 1, (size_t) (along * (float) samples.size()))]; };
            const auto value = read (from) + morph * (read (to) - read (from));
            const juce::Point<float> point (box.getX() + along * box.getWidth(), box.getCentreY() - 0.45f * value * box.getHeight());

            if (i == 0)
                wave.startNewSubPath (point);
            else
                wave.lineTo (point);
        }

        return wave;
    }

    void rebuild()
    {
        frames.clear();

        if (table == nullptr || getWidth() <= 0)
            return;

        const auto total = table->getNumFrames();
        const auto shown = std::min (total, WavetableTint::framesDrawn);

        for (int k = 0; k < shown; ++k)
        {
            const auto frame = shown > 1 ? juce::roundToInt ((double) k * (total - 1) / (shown - 1)) : 0;
            const auto depth = total > 1 ? (float) frame / (float) (total - 1) : 0.0f;
            const auto samples = table->getFrameForDrawing (frame);
            frames.push_back (makeWave (samples, samples, 0.0f, getFrameBox (depth)));
        }
    }
};

//==============================================================================
// An oscillator's picture: the button that names its table, over the drawing of it.
class WavetableEditor::Oscillator : public juce::Component
{
public:
    Oscillator (WavetableEditor& editor, int osc)
        : view (editor, osc, editor.getDevice().getParameter (getParameterID (osc, WavetableSynth::oscPosition)))
    {
        setTitle (getOscillatorName (osc) + " table");
        button.setTitle (getOscillatorName (osc) + " Wavetable Choice");
        button.setTooltip ("Chooses the table this oscillator plays: one that comes with Phazite, or a sound file of yours");
        button.onClick = [&editor, osc, this]
        { editor.getTableMenu (osc).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&button)); };
        addAndMakeVisible (button);
        addAndMakeVisible (view);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        button.setBounds (area.removeFromTop (controlHeight));
        area.removeFromTop (Space::xs);
        view.setBounds (area);
    }

    juce::TextButton button;
    TableView view;
};

//==============================================================================
// The modulation matrix: each slot a row, its source and target picked from lists, and its amount a knob.
class WavetableEditor::Matrix : public juce::Component,
                                private DisplayTicker::Display
{
public:
    explicit Matrix (const DeviceHandle& device)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this))
    {
        setTitle ("Modulation matrix");

        for (int slot = 0; slot < WavetableSynth::numModSlots; ++slot)
        {
            auto& row = rows.emplace_back (std::make_unique<Row>());
            const auto id = [slot] (WavetableSynth::ModField field)
            { return getParameterID (WavetableSynth::getModParameter (slot, field)); };
            const auto& amount =
                WavetableSynth::info.parameters[(size_t) WavetableSynth::getModParameter (slot, WavetableSynth::modAmount)];

            row->source.setTitle ("Mod " + juce::String (slot + 1) + " Source");
            row->target.setTitle ("Mod " + juce::String (slot + 1) + " Target");
            row->sourceAttachment.setParameter (device.getParameter (id (WavetableSynth::modSource)));
            row->targetAttachment.setParameter (device.getParameter (id (WavetableSynth::modTarget)));
            row->amount = std::make_unique<Knob> (juce::String (amount.name.data(), amount.name.size()), amount.defaultValue);
            showAsParameter (*row->amount, amount);
            row->amountAttachment = std::make_unique<ParameterAttachment> (*row->amount);
            row->amountAttachment->setParameter (device.getParameter (amount.id));
            row->number.setText (juce::String (slot + 1), juce::dontSendNotification);
            row->number.setFont (juce::FontOptions (TextSize::small));
            row->number.setColour (juce::Label::textColourId, getPalette (*this).textMuted);

            for (auto* control : std::initializer_list<juce::Component*> { &row->number, &row->source, &row->target, row->amount.get() })
                addAndMakeVisible (control);
        }

        tick();
    }

    juce::Component* findControl (std::string_view parameterID)
    {
        for (int slot = 0; slot < WavetableSynth::numModSlots; ++slot)
        {
            auto& row = *rows[(size_t) slot];

            if (parameterID == getParameterID (WavetableSynth::getModParameter (slot, WavetableSynth::modSource)))
                return &row.source;

            if (parameterID == getParameterID (WavetableSynth::getModParameter (slot, WavetableSynth::modTarget)))
                return &row.target;

            if (parameterID == getParameterID (WavetableSynth::getModParameter (slot, WavetableSynth::modAmount)))
                return row.amount.get();
        }

        return nullptr;
    }

    static constexpr int rowHeight = 26;
    static int getPreferredHeight() { return WavetableSynth::numModSlots * rowHeight; }
    static int getPreferredWidth() { return 16 + 2 * 96 + rowHeight + 2 * Space::xs; }

    void resized() override
    {
        auto area = getLocalBounds();

        for (auto& row : rows)
        {
            auto line = area.removeFromTop (rowHeight).reduced (0, 1);
            row->number.setBounds (line.removeFromLeft (16));
            row->amount->setBounds (line.removeFromRight (line.getHeight()));
            line.removeFromRight (Space::xs);
            const auto half = (line.getWidth() - Space::xs) / 2;
            row->source.setBounds (line.removeFromLeft (half));
            line.removeFromLeft (Space::xs);
            row->target.setBounds (line);
        }
    }

private:
    struct Row
    {
        juce::Label number;
        juce::ComboBox source, target;
        ChoiceAttachment sourceAttachment { source }, targetAttachment { target };
        std::unique_ptr<Knob> amount;
        std::unique_ptr<ParameterAttachment> amountAttachment;
    };

    std::vector<std::unique_ptr<Row>> rows;

    void tick() override
    {
        for (auto& row : rows)
        {
            row->sourceAttachment.update();
            row->targetAttachment.update();
            row->amountAttachment->update();
        }
    }
};

//==============================================================================
WavetableEditor::WavetableEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    const auto parameter = [this] (int index) { return device.getParameter (getParameterID (index)); };
    const auto shown = [this]() -> const WavetableSynth::Shown*
    {
        auto* synth = device.getProcessor<WavetableSynth>();
        return synth != nullptr ? &synth->getShown() : nullptr;
    };

    // The oscillators: each its table, then its controls.
    std::vector<DeviceLayout::Part> oscillatorParts;

    for (int osc = 0; osc < numOscillators; ++osc)
    {
        auto& picture = keep (std::make_unique<Oscillator> (*this, osc));
        views[(size_t) osc] = &picture.view;
        tableButtons[(size_t) osc] = &picture.button;

        std::vector<std::string_view> ids;

        for (const auto field : { WavetableSynth::oscOn, WavetableSynth::oscPosition, WavetableSynth::oscPitch, WavetableSynth::oscFine,
                                  WavetableSynth::oscLevel, WavetableSynth::oscWarp, WavetableSynth::oscWarpAmount })
            ids.push_back (getParameterID (osc, field));

        oscillatorParts.push_back (makePicturePart (getOscillatorName (osc), picture, 120, 44, ids, 4, ParameterControls::Size::compact,
                                                    { "On", "Position", "Pitch", "Fine", "Level", "Warp", "Amount" }));
    }

    const auto subAndNoise = makeControlsPart ("Sub and Noise", { "subLevel", "subOctave", "subShape", "noiseLevel" }, 4,
                                               ParameterControls::Size::compact, { "Sub", "Octave", "Shape", "Noise" });
    const auto unison = makeControlsPart ("Unison", { "unisonVoices", "unisonDetune", "unisonSpread", "unisonPosition" }, 4,
                                          ParameterControls::Size::compact, { "Voices", "Detune", "Spread", "Position" });

    // The filter: its response, then its controls.
    auto all = std::make_shared<std::vector<ParameterHandle>>();

    for (const auto& each : WavetableSynth::info.parameters)
        all->push_back (device.getParameter (each.id));

    auto& filterView = keep (std::make_unique<FilterView> (
        "Filter response", parameter (WavetableSynth::cutoff), parameter (WavetableSynth::resonance),
        [all]
        {
            std::vector<float> values;

            for (const auto& each : *all)
                values.push_back (each.getValue());

            return values;
        },
        [this] (const std::vector<float>& values, double cutoffHertz, double frequency)
        {
            auto* scope = device.getScope();
            return WavetableSynth::getFilterMagnitude (values, cutoffHertz, frequency, scope != nullptr ? scope->getSampleRate() : 48000.0);
        },
        [shown] { return shown() != nullptr ? shown()->cutoff.get() : 0.0f; }));
    const auto filter = makePicturePart (
        "Filter", filterView, 80, 40, { "filterOn", "filterType", "cutoff", "resonance", "filterSlope", "drive", "keyTracking" }, 4,
        ParameterControls::Size::compact, { "On", "Type", "Cutoff", "Resonance", "Slope", "Drive", "Key" });

    // The modulation: the envelopes, the LFOs, the matrix and how it plays.
    std::vector<DeviceLayout::Part> modulation;

    for (int envelope = 0; envelope < 3; ++envelope)
    {
        const auto first = WavetableSynth::env1Attack + 4 * envelope;
        auto& view = keep (std::make_unique<EnvelopeView> (
            "Envelope " + juce::String (envelope + 1), parameter (first), parameter (first + 1), parameter (first + 2),
            parameter (first + 3), [shown, envelope] { return shown() != nullptr ? shown()->envelopes[(size_t) envelope].get() : 0.0f; }));
        const auto name = envelope == 0 ? juce::String ("Amp") : "Envelope " + juce::String (envelope + 1);
        modulation.push_back (
            makePicturePart (name, view, 64, 36,
                             { getParameterID (first), getParameterID (first + 1), getParameterID (first + 2), getParameterID (first + 3) },
                             4, ParameterControls::Size::compact, { "Attack", "Decay", "Sustain", "Release" }));
    }

    for (int lfo = 0; lfo < 2; ++lfo)
    {
        auto lfoShape = std::make_shared<ParameterHandle> (parameter (WavetableSynth::getLfoParameter (lfo, WavetableSynth::lfoShape)));
        auto& view = keep (std::make_unique<LfoView> (
            "LFO " + juce::String (lfo + 1),
            [lfoShape] (float phase) { return getDrawnLfoValue (juce::roundToInt (lfoShape->getValue()), phase); },
            [shown, lfo] { return shown() != nullptr ? shown()->lfoPhases[(size_t) lfo].get() : -1.0f; }));

        std::vector<std::string_view> ids;

        for (const auto field : { WavetableSynth::lfoShape, WavetableSynth::lfoRate, WavetableSynth::lfoSync, WavetableSynth::lfoDivision,
                                  WavetableSynth::lfoRetrigger })
            ids.push_back (getParameterID (WavetableSynth::getLfoParameter (lfo, field)));

        modulation.push_back (makePicturePart ("LFO " + juce::String (lfo + 1), view, 64, 36, ids, 3, ParameterControls::Size::compact,
                                               { "Shape", "Rate", "Sync", "Division", "Retrigger" }));
    }

    auto& matrix = keep (std::make_unique<Matrix> (device));

    for (int slot = 0; slot < WavetableSynth::numModSlots; ++slot)
        for (const auto field : { WavetableSynth::modSource, WavetableSynth::modTarget, WavetableSynth::modAmount })
            if (auto* control = matrix.findControl (getParameterID (WavetableSynth::getModParameter (slot, field))))
                addControl (getParameterID (WavetableSynth::getModParameter (slot, field)), *control);

    modulation.push_back ({ "Matrix", &matrix, Matrix::getPreferredWidth(), Matrix::getPreferredHeight(), Matrix::getPreferredHeight() });
    modulation.push_back (makeControlsPart ("Voice", { "voiceMode", "glide", "bendRange", "velocity", "level" }, 3,
                                            ParameterControls::Size::compact, { "Mode", "Glide", "Bend", "Velocity", "Level" }));

    oscillatorParts.push_back (subAndNoise);
    oscillatorParts.push_back (unison);
    setParts ({ oscillatorParts, { filter }, modulation }, { 2 });
    refresh();
}

WavetableEditor::~WavetableEditor() = default;

juce::Component& WavetableEditor::getTableView (int osc) const
{
    return *views[(size_t) std::clamp (osc, 0, numOscillators - 1)];
}

juce::Button& WavetableEditor::getTableButton (int osc) const
{
    return *tableButtons[(size_t) std::clamp (osc, 0, numOscillators - 1)];
}

juce::PopupMenu WavetableEditor::getTableMenu (int osc)
{
    juce::PopupMenu menu;
    const auto current = device.getWavetableName (osc);
    const auto fromFile = device.hasWavetableFile (osc);
    const juce::Component::SafePointer<WavetableEditor> safe (this);

    // A sound file of the user's, ticked while it plays.
    if (fromFile)
    {
        menu.addItem (current, false, true, nullptr);
        menu.addSeparator();
    }

    const auto names = devices::getFactoryWavetableNames();

    for (int index = 0; index < (int) names.size(); ++index)
    {
        const juce::String name (names[(size_t) index].data(), names[(size_t) index].size());
        menu.addItem (name, true, ! fromFile && name == current,
                      [safe, osc, index]
                      {
                          if (safe != nullptr)
                          {
                              safe->device.setFactoryWavetable (osc, index);
                              safe->refresh();
                          }
                      });
    }

    menu.addSeparator();
    menu.addItem ("Load Wavetable File...",
                  [safe, osc]
                  {
                      if (safe != nullptr)
                          safe->chooseFile (osc);
                  });
    return menu;
}

bool WavetableEditor::dropFile (int osc, const juce::File& file)
{
    const auto put = device.setWavetableFile (osc, file);
    refresh();
    return put;
}

void WavetableEditor::chooseFile (int osc)
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a wavetable for " + getOscillatorName (osc), juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<WavetableEditor> (this), osc] (const juce::FileChooser& chosen)
                          {
                              if (safe != nullptr && chosen.getResult() != juce::File())
                                  safe->dropFile (osc, chosen.getResult());
                          });
}

void WavetableEditor::refresh()
{
    auto* synth = device.getProcessor<WavetableSynth>();

    for (int osc = 0; osc < numOscillators; ++osc)
    {
        const auto name = device.getWavetableName (osc);
        tableButtons[(size_t) osc]->setButtonText (name);
        views[(size_t) osc]->setTable (device.getWavetable (osc), name);

        const auto on = device.getParameter (getParameterID (osc, WavetableSynth::oscOn)).getValue() > 0.5f;
        const auto position = synth != nullptr ? synth->getPlayedPosition (osc)
                                               : device.getParameter (getParameterID (osc, WavetableSynth::oscPosition)).getValue();
        views[(size_t) osc]->setPlayed (position, on);
    }
}
} // namespace phazite::ui
